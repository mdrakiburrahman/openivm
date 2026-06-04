-- {"operators": "DUCKLAKE,CTE,UNNEST,FILTER,PROJECTION,INNER_JOIN,LEFT_JOIN,AGGREGATE,ORDER", "complexity": "high", "is_incremental": true, "has_nulls": true, "has_cast": true, "has_case": true, "tables": "CUSTOMER,OORDER,ORDER_LINE,HISTORY,DISTRICT,WAREHOUSE", "ducklake": true}
WITH
params AS (
	SELECT
		'GC' AS good_credit,
		'BC' AS bad_credit,
		0.0::DOUBLE AS zero_amount,
		'orders' AS orders_bucket,
		'payments' AS payments_bucket,
		'balance' AS balance_bucket
),
warehouse_dim AS (
	SELECT
		w.W_ID AS warehouse_id,
		LOWER(TRIM(w.W_NAME)) AS warehouse_name,
		COALESCE(w.W_STATE, 'NA') AS warehouse_state,
		CAST(w.W_TAX AS DOUBLE) AS warehouse_tax
	FROM dl.WAREHOUSE w
	WHERE w.W_ID IS NOT NULL
),
district_dim AS (
	SELECT
		d.D_W_ID AS warehouse_id,
		d.D_ID AS district_id,
		LOWER(TRIM(d.D_NAME)) AS district_name,
		CAST(d.D_TAX AS DOUBLE) AS district_tax,
		d.D_NEXT_O_ID AS next_order_id
	FROM dl.DISTRICT d
	WHERE d.D_ID BETWEEN 1 AND 10
),
customer_base AS (
	SELECT
		c.C_W_ID AS warehouse_id,
		c.C_D_ID AS district_id,
		c.C_ID AS customer_id,
		COALESCE(NULLIF(TRIM(c.C_LAST), ''), 'unknown') AS customer_last,
		COALESCE(c.C_CREDIT, (SELECT bad_credit FROM params)) AS credit_code,
		CAST(c.C_BALANCE AS DOUBLE) AS balance_amount,
		CAST(c.C_YTD_PAYMENT AS DOUBLE) AS ytd_payment,
		CAST(c.C_PAYMENT_CNT AS BIGINT) AS payment_count,
		CAST(c.C_DELIVERY_CNT AS BIGINT) AS delivery_count
	FROM dl.CUSTOMER c
	WHERE c.C_ID > 0
),
customer_dim AS (
	SELECT
		cb.warehouse_id,
		cb.district_id,
		cb.customer_id,
		cb.customer_last,
		cb.credit_code,
		cb.balance_amount,
		cb.ytd_payment,
		cb.payment_count,
		cb.delivery_count,
		dd.district_name,
		wd.warehouse_state
	FROM customer_base cb
	JOIN district_dim dd
		ON dd.warehouse_id = cb.warehouse_id
	   AND dd.district_id = cb.district_id
	JOIN warehouse_dim wd
		ON wd.warehouse_id = cb.warehouse_id
),
orders_base AS (
	SELECT
		o.O_W_ID AS warehouse_id,
		o.O_D_ID AS district_id,
		o.O_C_ID AS customer_id,
		o.O_ID AS order_id,
		COALESCE(o.O_CARRIER_ID, -1) AS carrier_id
	FROM dl.OORDER o
	WHERE o.O_ID IS NOT NULL
),
order_line_base AS (
	SELECT
		ol.OL_W_ID AS warehouse_id,
		ol.OL_D_ID AS district_id,
		ol.OL_O_ID AS order_id,
		CAST(ol.OL_AMOUNT AS DOUBLE) AS line_amount,
		CAST(ol.OL_QUANTITY AS BIGINT) AS quantity
	FROM dl.ORDER_LINE ol
	WHERE ol.OL_NUMBER > 0
),
order_rollup AS (
	SELECT
		ob.warehouse_id,
		ob.district_id,
		ob.customer_id,
		COUNT(DISTINCT ob.order_id) AS order_count,
		SUM(olb.line_amount) AS order_amount,
		SUM(olb.quantity) AS ordered_quantity,
		SUM(CASE WHEN ob.carrier_id = -1 THEN 1 ELSE 0 END) AS open_order_lines
	FROM orders_base ob
	JOIN order_line_base olb
		ON olb.warehouse_id = ob.warehouse_id
	   AND olb.district_id = ob.district_id
	   AND olb.order_id = ob.order_id
	GROUP BY
		ob.warehouse_id,
		ob.district_id,
		ob.customer_id
),
history_rollup AS (
	SELECT
		h.H_C_W_ID AS warehouse_id,
		h.H_C_D_ID AS district_id,
		h.H_C_ID AS customer_id,
		SUM(CAST(h.H_AMOUNT AS DOUBLE)) AS history_amount,
		COUNT(*) AS history_rows
	FROM dl.HISTORY h
	GROUP BY
		h.H_C_W_ID,
		h.H_C_D_ID,
		h.H_C_ID
),
customer_metrics AS (
	SELECT
		cd.warehouse_id,
		cd.district_id,
		cd.customer_id,
		cd.customer_last,
		cd.credit_code,
		cd.balance_amount,
		cd.ytd_payment,
		cd.payment_count,
		cd.delivery_count,
		cd.district_name,
		cd.warehouse_state,
		COALESCE(orr.order_count, 0) AS order_count,
		COALESCE(orr.order_amount, 0.0) AS order_amount,
		COALESCE(orr.ordered_quantity, 0) AS ordered_quantity,
		COALESCE(orr.open_order_lines, 0) AS open_order_lines,
		COALESCE(hr.history_amount, 0.0) AS history_amount,
		COALESCE(hr.history_rows, 0) AS history_rows
	FROM customer_dim cd
	LEFT JOIN order_rollup orr
		ON orr.warehouse_id = cd.warehouse_id
	   AND orr.district_id = cd.district_id
	   AND orr.customer_id = cd.customer_id
	LEFT JOIN history_rollup hr
		ON hr.warehouse_id = cd.warehouse_id
	   AND hr.district_id = cd.district_id
	   AND hr.customer_id = cd.customer_id
),
metric_unpivot AS (
	SELECT
		cm.warehouse_id,
		cm.district_id,
		cm.customer_id,
		cm.credit_code,
		cm.customer_last,
		cm.district_name,
		cm.warehouse_state,
		cm.balance_amount,
		cm.ytd_payment,
		cm.payment_count,
		cm.order_count,
		cm.order_amount,
		cm.ordered_quantity,
		cm.open_order_lines,
		cm.history_amount,
		cm.history_rows,
		unnest(['orders', 'payments', 'balance', 'skip']) AS metric_bucket
	FROM customer_metrics cm
),
metric_filtered AS (
	SELECT
		mu.*,
		CASE
			WHEN mu.metric_bucket = (SELECT orders_bucket FROM params) THEN mu.order_amount
			WHEN mu.metric_bucket = (SELECT payments_bucket FROM params) THEN mu.history_amount
			WHEN mu.metric_bucket = (SELECT balance_bucket FROM params) THEN mu.balance_amount
			ELSE (SELECT zero_amount FROM params)
		END AS metric_value,
		CASE
			WHEN mu.metric_bucket = (SELECT orders_bucket FROM params) THEN mu.order_count
			WHEN mu.metric_bucket = (SELECT payments_bucket FROM params) THEN mu.history_rows
			WHEN mu.metric_bucket = (SELECT balance_bucket FROM params) THEN mu.payment_count
			ELSE 0
		END AS metric_rows
	FROM metric_unpivot mu
	WHERE mu.metric_bucket <> 'skip'
),
metric_named AS (
	SELECT
		mf.warehouse_id,
		mf.district_id,
		mf.credit_code,
		mf.metric_bucket,
		mf.district_name,
		mf.warehouse_state,
		mf.metric_value,
		mf.metric_rows,
		mf.open_order_lines,
		CASE
			WHEN mf.metric_value < 0 THEN 'negative'
			WHEN mf.metric_value = 0 THEN 'zero'
			ELSE 'positive'
		END AS metric_sign
	FROM metric_filtered mf
	WHERE mf.metric_rows >= 0
),
rollup AS (
	SELECT
		mn.warehouse_id,
		mn.district_id,
		mn.warehouse_state,
		mn.district_name,
		mn.credit_code,
		mn.metric_bucket,
		mn.metric_sign,
		COUNT(*) AS row_count,
		SUM(mn.metric_value) AS metric_value_sum,
		SUM(mn.metric_rows) AS metric_row_sum,
		SUM(mn.open_order_lines) AS open_line_sum,
		MIN(mn.metric_value) AS min_metric_value,
		MAX(mn.metric_value) AS max_metric_value
	FROM metric_named mn
	GROUP BY
		mn.warehouse_id,
		mn.district_id,
		mn.warehouse_state,
		mn.district_name,
		mn.credit_code,
		mn.metric_bucket,
		mn.metric_sign
),
final_rows AS (
	SELECT
		r.warehouse_id,
		r.district_id,
		r.warehouse_state,
		r.district_name,
		r.credit_code,
		r.metric_bucket,
		r.metric_sign,
		r.row_count,
		r.metric_value_sum,
		r.metric_row_sum,
		r.open_line_sum,
		r.min_metric_value,
		r.max_metric_value
	FROM rollup r
	WHERE r.row_count > 0
)
SELECT
	warehouse_id,
	district_id,
	warehouse_state,
	district_name,
	credit_code,
	metric_bucket,
	metric_sign,
	row_count,
	metric_value_sum,
	metric_row_sum,
	open_line_sum,
	min_metric_value,
	max_metric_value
FROM final_rows
ORDER BY
	warehouse_id,
	district_id,
	credit_code,
	metric_bucket,
	metric_sign
