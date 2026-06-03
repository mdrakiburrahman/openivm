-- {"operators": "DUCKLAKE,CTE,VALUES,CONSTANT,UNION,INNER_JOIN,LEFT_JOIN,AGGREGATE,HAVING,ORDER", "complexity": "high", "is_incremental": true, "has_nulls": true, "has_cast": true, "has_case": true, "tables": "WAREHOUSE,DISTRICT,CUSTOMER,OORDER,ORDER_LINE,HISTORY", "ducklake": true}
WITH
params AS (
	SELECT
		0.0::DOUBLE AS zero_amount,
		'GC' AS good_credit,
		'BC' AS bad_credit
),
metric_catalog AS (
	SELECT * FROM (
		VALUES
			('balance', 1, 'customer'),
			('payment', 2, 'customer'),
			('orders', 3, 'order'),
			('history', 4, 'history')
	) AS v(metric_name, metric_rank, metric_family)
),
warehouse_dim AS (
	SELECT
		w.W_ID AS warehouse_id,
		LOWER(TRIM(w.W_NAME)) AS warehouse_name,
		COALESCE(w.W_STATE, 'NA') AS warehouse_state
	FROM dl.WAREHOUSE w
	WHERE w.W_ID IS NOT NULL
),
district_dim AS (
	SELECT
		d.D_W_ID AS warehouse_id,
		d.D_ID AS district_id,
		LOWER(TRIM(d.D_NAME)) AS district_name,
		d.D_NEXT_O_ID AS next_order_id
	FROM dl.DISTRICT d
	WHERE d.D_ID BETWEEN 1 AND 10
),
customer_dim AS (
	SELECT
		c.C_W_ID AS warehouse_id,
		c.C_D_ID AS district_id,
		c.C_ID AS customer_id,
		COALESCE(c.C_CREDIT, (SELECT bad_credit FROM params)) AS credit_code,
		CAST(c.C_BALANCE AS DOUBLE) AS balance_amount,
		CAST(c.C_YTD_PAYMENT AS DOUBLE) AS ytd_payment,
		CAST(c.C_PAYMENT_CNT AS BIGINT) AS payment_count,
		CAST(c.C_DELIVERY_CNT AS BIGINT) AS delivery_count
	FROM dl.CUSTOMER c
	WHERE c.C_ID > 0
),
customer_enriched AS (
	SELECT
		cd.warehouse_id,
		cd.district_id,
		cd.customer_id,
		cd.credit_code,
		cd.balance_amount,
		cd.ytd_payment,
		cd.payment_count,
		cd.delivery_count,
		dd.district_name,
		wd.warehouse_state
	FROM customer_dim cd
	JOIN district_dim dd
		ON dd.warehouse_id = cd.warehouse_id
	   AND dd.district_id = cd.district_id
	JOIN warehouse_dim wd
		ON wd.warehouse_id = cd.warehouse_id
),
order_header AS (
	SELECT
		o.O_W_ID AS warehouse_id,
		o.O_D_ID AS district_id,
		o.O_C_ID AS customer_id,
		o.O_ID AS order_id,
		COALESCE(o.O_CARRIER_ID, -1) AS carrier_id
	FROM dl.OORDER o
	WHERE o.O_ID IS NOT NULL
),
line_base AS (
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
		oh.warehouse_id,
		oh.district_id,
		oh.customer_id,
		COUNT(DISTINCT oh.order_id) AS order_rows,
		SUM(lb.line_amount) AS order_amount,
		SUM(lb.quantity) AS ordered_quantity,
		SUM(CASE WHEN oh.carrier_id = -1 THEN 1 ELSE 0 END) AS open_line_rows
	FROM order_header oh
	JOIN line_base lb
		ON lb.warehouse_id = oh.warehouse_id
	   AND lb.district_id = oh.district_id
	   AND lb.order_id = oh.order_id
	GROUP BY
		oh.warehouse_id,
		oh.district_id,
		oh.customer_id
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
customer_activity AS (
	SELECT
		ce.warehouse_id,
		ce.district_id,
		ce.customer_id,
		ce.credit_code,
		ce.balance_amount,
		ce.ytd_payment,
		ce.payment_count,
		ce.delivery_count,
		ce.district_name,
		ce.warehouse_state,
		COALESCE(orr.order_rows, 0) AS order_rows,
		COALESCE(orr.order_amount, 0.0) AS order_amount,
		COALESCE(orr.ordered_quantity, 0) AS ordered_quantity,
		COALESCE(orr.open_line_rows, 0) AS open_line_rows,
		COALESCE(hr.history_amount, 0.0) AS history_amount,
		COALESCE(hr.history_rows, 0) AS history_rows
	FROM customer_enriched ce
	LEFT JOIN order_rollup orr
		ON orr.warehouse_id = ce.warehouse_id
	   AND orr.district_id = ce.district_id
	   AND orr.customer_id = ce.customer_id
	LEFT JOIN history_rollup hr
		ON hr.warehouse_id = ce.warehouse_id
	   AND hr.district_id = ce.district_id
	   AND hr.customer_id = ce.customer_id
),
balance_branch AS (
	SELECT
		ca.warehouse_id,
		ca.district_id,
		ca.warehouse_state,
		ca.district_name,
		ca.credit_code,
		mc.metric_name,
		mc.metric_rank,
		mc.metric_family,
		ca.balance_amount AS metric_value,
		1::BIGINT AS metric_rows,
		CASE WHEN ca.balance_amount < 0 THEN 'negative' ELSE 'non_negative' END AS metric_bucket
	FROM customer_activity ca
	JOIN metric_catalog mc
		ON mc.metric_name = 'balance'
),
payment_branch AS (
	SELECT
		ca.warehouse_id,
		ca.district_id,
		ca.warehouse_state,
		ca.district_name,
		ca.credit_code,
		mc.metric_name,
		mc.metric_rank,
		mc.metric_family,
		ca.ytd_payment AS metric_value,
		ca.payment_count AS metric_rows,
		CASE WHEN ca.payment_count = 0 THEN 'no_payment' ELSE 'paid' END AS metric_bucket
	FROM customer_activity ca
	JOIN metric_catalog mc
		ON mc.metric_name = 'payment'
),
order_branch AS (
	SELECT
		ca.warehouse_id,
		ca.district_id,
		ca.warehouse_state,
		ca.district_name,
		ca.credit_code,
		mc.metric_name,
		mc.metric_rank,
		mc.metric_family,
		ca.order_amount AS metric_value,
		ca.order_rows AS metric_rows,
		CASE WHEN ca.open_line_rows > 0 THEN 'open_order' ELSE 'closed_order' END AS metric_bucket
	FROM customer_activity ca
	JOIN metric_catalog mc
		ON mc.metric_name = 'orders'
),
history_branch AS (
	SELECT
		ca.warehouse_id,
		ca.district_id,
		ca.warehouse_state,
		ca.district_name,
		ca.credit_code,
		mc.metric_name,
		mc.metric_rank,
		mc.metric_family,
		ca.history_amount AS metric_value,
		ca.history_rows AS metric_rows,
		CASE WHEN ca.history_rows > 0 THEN 'history' ELSE 'no_history' END AS metric_bucket
	FROM customer_activity ca
	JOIN metric_catalog mc
		ON mc.metric_name = 'history'
),
metric_union AS (
	SELECT * FROM balance_branch
	UNION ALL
	SELECT * FROM payment_branch
	UNION ALL
	SELECT * FROM order_branch
	UNION ALL
	SELECT * FROM history_branch
),
rollup AS (
	SELECT
		mu.warehouse_id,
		mu.district_id,
		mu.warehouse_state,
		mu.district_name,
		mu.credit_code,
		mu.metric_name,
		mu.metric_rank,
		mu.metric_family,
		mu.metric_bucket,
		COUNT(*) AS row_count,
		SUM(mu.metric_value) AS metric_value_sum,
		SUM(mu.metric_rows) AS metric_row_sum,
		MIN(mu.metric_value) AS min_metric_value,
		MAX(mu.metric_value) AS max_metric_value
	FROM metric_union mu
	GROUP BY
		mu.warehouse_id,
		mu.district_id,
		mu.warehouse_state,
		mu.district_name,
		mu.credit_code,
		mu.metric_name,
		mu.metric_rank,
		mu.metric_family,
		mu.metric_bucket
	HAVING COUNT(*) > 0
),
final_rows AS (
	SELECT
		r.warehouse_id,
		r.district_id,
		r.warehouse_state,
		r.district_name,
		r.credit_code,
		r.metric_name,
		r.metric_rank,
		r.metric_family,
		r.metric_bucket,
		r.row_count,
		COALESCE(r.metric_value_sum, (SELECT zero_amount FROM params)) AS metric_value_sum,
		r.metric_row_sum,
		r.min_metric_value,
		r.max_metric_value
	FROM rollup r
)
SELECT
	warehouse_id,
	district_id,
	warehouse_state,
	district_name,
	credit_code,
	metric_name,
	metric_rank,
	metric_family,
	metric_bucket,
	row_count,
	metric_value_sum,
	metric_row_sum,
	min_metric_value,
	max_metric_value
FROM final_rows
ORDER BY
	warehouse_id,
	district_id,
	credit_code,
	metric_rank,
	metric_bucket
