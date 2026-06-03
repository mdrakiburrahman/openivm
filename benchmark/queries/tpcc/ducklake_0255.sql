-- {"operators": "DUCKLAKE,CTE,NULL_SAFE_JOIN,LEFT_JOIN,FULL_OUTER_JOIN,AGGREGATE,HAVING,ORDER", "complexity": "high", "is_incremental": true, "has_nulls": true, "has_cast": true, "has_case": true, "tables": "CUSTOMER,HISTORY,OORDER,ORDER_LINE,DISTRICT,WAREHOUSE", "ducklake": true}
WITH
params AS (
	SELECT
		'GC' AS good_credit,
		'BC' AS bad_credit,
		'no_credit' AS missing_credit,
		0.0::DOUBLE AS zero_amount
),
warehouse_dim AS (
	SELECT
		w.W_ID AS warehouse_id,
		COALESCE(w.W_STATE, 'NA') AS warehouse_state,
		LOWER(TRIM(w.W_NAME)) AS warehouse_name
	FROM dl.WAREHOUSE w
	WHERE w.W_ID IS NOT NULL
),
district_dim AS (
	SELECT
		d.D_W_ID AS warehouse_id,
		d.D_ID AS district_id,
		LOWER(TRIM(d.D_NAME)) AS district_name,
		CAST(d.D_YTD AS DOUBLE) AS district_ytd
	FROM dl.DISTRICT d
	WHERE d.D_ID BETWEEN 1 AND 10
),
customer_base AS (
	SELECT
		c.C_W_ID AS warehouse_id,
		c.C_D_ID AS district_id,
		c.C_ID AS customer_id,
		COALESCE(c.C_CREDIT, (SELECT missing_credit FROM params)) AS credit_code,
		COALESCE(NULLIF(TRIM(c.C_LAST), ''), 'unknown') AS customer_last,
		CAST(c.C_BALANCE AS DOUBLE) AS balance_amount,
		CAST(c.C_YTD_PAYMENT AS DOUBLE) AS ytd_payment,
		CAST(c.C_PAYMENT_CNT AS BIGINT) AS payment_count
	FROM dl.CUSTOMER c
	WHERE c.C_ID > 0
),
customer_enriched AS (
	SELECT
		cb.warehouse_id,
		cb.district_id,
		cb.customer_id,
		cb.credit_code,
		cb.customer_last,
		cb.balance_amount,
		cb.ytd_payment,
		cb.payment_count,
		dd.district_name,
		wd.warehouse_state
	FROM customer_base cb
	LEFT JOIN district_dim dd
		ON dd.warehouse_id IS NOT DISTINCT FROM cb.warehouse_id
	   AND dd.district_id IS NOT DISTINCT FROM cb.district_id
	LEFT JOIN warehouse_dim wd
		ON wd.warehouse_id IS NOT DISTINCT FROM cb.warehouse_id
),
history_base AS (
	SELECT
		h.H_C_W_ID AS warehouse_id,
		h.H_C_D_ID AS district_id,
		h.H_C_ID AS customer_id,
		CAST(h.H_AMOUNT AS DOUBLE) AS history_amount,
		LOWER(COALESCE(h.H_DATA, '')) AS history_data
	FROM dl.HISTORY h
	WHERE h.H_C_ID IS NOT NULL
),
history_grouped AS (
	SELECT
		hb.warehouse_id,
		hb.district_id,
		hb.customer_id,
		SUM(hb.history_amount) AS history_sum,
		COUNT(*) AS history_rows,
		SUM(CASE WHEN hb.history_data LIKE '%payment%' THEN 1 ELSE 0 END) AS payment_rows
	FROM history_base hb
	GROUP BY
		hb.warehouse_id,
		hb.district_id,
		hb.customer_id
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
order_grouped AS (
	SELECT
		ob.warehouse_id,
		ob.district_id,
		ob.customer_id,
		COUNT(DISTINCT ob.order_id) AS order_rows,
		SUM(lb.line_amount) AS order_amount,
		SUM(lb.quantity) AS order_quantity,
		SUM(CASE WHEN ob.carrier_id = -1 THEN 1 ELSE 0 END) AS open_line_rows
	FROM orders_base ob
	JOIN line_base lb
		ON lb.warehouse_id = ob.warehouse_id
	   AND lb.district_id = ob.district_id
	   AND lb.order_id = ob.order_id
	GROUP BY
		ob.warehouse_id,
		ob.district_id,
		ob.customer_id
),
customer_history AS (
	SELECT
		COALESCE(ce.warehouse_id, hg.warehouse_id) AS warehouse_id,
		COALESCE(ce.district_id, hg.district_id) AS district_id,
		COALESCE(ce.customer_id, hg.customer_id) AS customer_id,
		ce.credit_code,
		ce.customer_last,
		ce.balance_amount,
		ce.ytd_payment,
		ce.payment_count,
		ce.district_name,
		ce.warehouse_state,
		COALESCE(hg.history_sum, (SELECT zero_amount FROM params)) AS history_sum,
		COALESCE(hg.history_rows, 0) AS history_rows,
		COALESCE(hg.payment_rows, 0) AS payment_rows,
		CASE
			WHEN ce.customer_id IS NULL THEN 'history_only'
			WHEN hg.customer_id IS NULL THEN 'customer_only'
			ELSE 'matched'
		END AS history_match_state
	FROM customer_enriched ce
	FULL OUTER JOIN history_grouped hg
		ON hg.warehouse_id IS NOT DISTINCT FROM ce.warehouse_id
	   AND hg.district_id IS NOT DISTINCT FROM ce.district_id
	   AND hg.customer_id IS NOT DISTINCT FROM ce.customer_id
),
activity_joined AS (
	SELECT
		COALESCE(ch.warehouse_id, og.warehouse_id) AS warehouse_id,
		COALESCE(ch.district_id, og.district_id) AS district_id,
		COALESCE(ch.customer_id, og.customer_id) AS customer_id,
		COALESCE(ch.credit_code, (SELECT missing_credit FROM params)) AS credit_code,
		COALESCE(ch.warehouse_state, 'NA') AS warehouse_state,
		COALESCE(ch.district_name, 'missing') AS district_name,
		COALESCE(ch.balance_amount, (SELECT zero_amount FROM params)) AS balance_amount,
		COALESCE(ch.ytd_payment, (SELECT zero_amount FROM params)) AS ytd_payment,
		COALESCE(ch.history_sum, (SELECT zero_amount FROM params)) AS history_sum,
		COALESCE(ch.history_rows, 0) AS history_rows,
		COALESCE(ch.payment_rows, 0) AS payment_rows,
		COALESCE(og.order_rows, 0) AS order_rows,
		COALESCE(og.order_amount, (SELECT zero_amount FROM params)) AS order_amount,
		COALESCE(og.order_quantity, 0) AS order_quantity,
		COALESCE(og.open_line_rows, 0) AS open_line_rows,
		CASE
			WHEN ch.customer_id IS NULL THEN 'order_only'
			WHEN og.customer_id IS NULL THEN ch.history_match_state
			ELSE 'customer_order'
		END AS match_state
	FROM customer_history ch
	FULL OUTER JOIN order_grouped og
		ON og.warehouse_id IS NOT DISTINCT FROM ch.warehouse_id
	   AND og.district_id IS NOT DISTINCT FROM ch.district_id
	   AND og.customer_id IS NOT DISTINCT FROM ch.customer_id
),
state_labeled AS (
	SELECT
		aj.*,
		CASE
			WHEN aj.credit_code = (SELECT good_credit FROM params) THEN 'good'
			WHEN aj.credit_code = (SELECT bad_credit FROM params) THEN 'bad'
			ELSE 'missing'
		END AS credit_bucket,
		CASE
			WHEN aj.order_rows > 0 AND aj.history_rows > 0 THEN 'both'
			WHEN aj.order_rows > 0 THEN 'orders'
			WHEN aj.history_rows > 0 THEN 'history'
			ELSE 'none'
		END AS activity_bucket
	FROM activity_joined aj
),
rollup AS (
	SELECT
		sl.warehouse_id,
		sl.district_id,
		sl.warehouse_state,
		sl.district_name,
		sl.credit_bucket,
		sl.activity_bucket,
		sl.match_state,
		COUNT(*) AS row_count,
		SUM(sl.balance_amount) AS balance_sum,
		SUM(sl.ytd_payment) AS ytd_payment_sum,
		SUM(sl.history_sum) AS history_sum,
		SUM(sl.order_amount) AS order_amount_sum,
		SUM(sl.order_quantity) AS order_quantity_sum,
		SUM(sl.payment_rows) AS payment_rows_sum,
		SUM(sl.open_line_rows) AS open_line_rows_sum
	FROM state_labeled sl
	GROUP BY
		sl.warehouse_id,
		sl.district_id,
		sl.warehouse_state,
		sl.district_name,
		sl.credit_bucket,
		sl.activity_bucket,
		sl.match_state
	HAVING COUNT(*) > 0
),
final_rows AS (
	SELECT
		r.warehouse_id,
		r.district_id,
		r.warehouse_state,
		r.district_name,
		r.credit_bucket,
		r.activity_bucket,
		r.match_state,
		r.row_count,
		r.balance_sum,
		r.ytd_payment_sum,
		r.history_sum,
		r.order_amount_sum,
		r.order_quantity_sum,
		r.payment_rows_sum,
		r.open_line_rows_sum
	FROM rollup r
)
SELECT
	warehouse_id,
	district_id,
	warehouse_state,
	district_name,
	credit_bucket,
	activity_bucket,
	match_state,
	row_count,
	balance_sum,
	ytd_payment_sum,
	history_sum,
	order_amount_sum,
	order_quantity_sum,
	payment_rows_sum,
	open_line_rows_sum
FROM final_rows
ORDER BY
	warehouse_id,
	district_id,
	credit_bucket,
	activity_bucket,
	match_state
