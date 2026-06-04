-- {"operators": "DUCKLAKE,CTE,INNER_JOIN,LEFT_JOIN,AGGREGATE,FILTER,HAVING,ORDER", "complexity": "high", "is_incremental": true, "has_nulls": true, "has_cast": true, "has_case": true, "tables": "WAREHOUSE,DISTRICT,CUSTOMER,OORDER,ORDER_LINE,HISTORY", "ducklake": true}
WITH
params AS (
	SELECT
		DATE '2026-06-01' AS start_date,
		DATE '2026-06-30' AS end_date,
		'GC' AS good_credit,
		'BC' AS bad_credit,
		0.0::DOUBLE AS zero_amount,
		100.0::DOUBLE AS high_balance_cutoff
),
warehouse_base AS (
	SELECT
		w.W_ID AS warehouse_id,
		LOWER(TRIM(w.W_NAME)) AS warehouse_name,
		COALESCE(w.W_STATE, 'NA') AS warehouse_state,
		CAST(w.W_TAX AS DOUBLE) AS warehouse_tax,
		CAST(w.W_YTD AS DOUBLE) AS warehouse_ytd
	FROM dl.WAREHOUSE w
	WHERE w.W_ID IS NOT NULL
),
warehouse_flags AS (
	SELECT
		wb.*,
		CASE
			WHEN wb.warehouse_tax > 0.08 THEN 'high_tax'
			WHEN wb.warehouse_tax > 0.04 THEN 'mid_tax'
			ELSE 'low_tax'
		END AS warehouse_tax_band
	FROM warehouse_base wb
),
district_base AS (
	SELECT
		d.D_W_ID AS warehouse_id,
		d.D_ID AS district_id,
		LOWER(TRIM(d.D_NAME)) AS district_name,
		CAST(d.D_TAX AS DOUBLE) AS district_tax,
		CAST(d.D_YTD AS DOUBLE) AS district_ytd,
		d.D_NEXT_O_ID AS next_order_id
	FROM dl.DISTRICT d
	WHERE d.D_ID BETWEEN 1 AND 10
),
district_joined AS (
	SELECT
		db.warehouse_id,
		db.district_id,
		db.district_name,
		db.district_tax,
		db.district_ytd,
		db.next_order_id,
		wf.warehouse_state,
		wf.warehouse_tax_band
	FROM district_base db
	JOIN warehouse_flags wf
		ON wf.warehouse_id = db.warehouse_id
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
		CAST(c.C_PAYMENT_CNT AS BIGINT) AS payment_count
	FROM dl.CUSTOMER c
	WHERE c.C_ID > 0
),
customer_enriched AS (
	SELECT
		cb.warehouse_id,
		cb.district_id,
		cb.customer_id,
		cb.customer_last,
		cb.credit_code,
		cb.balance_amount,
		cb.ytd_payment,
		cb.payment_count,
		dj.district_name,
		dj.warehouse_state,
		dj.warehouse_tax_band
	FROM customer_base cb
	JOIN district_joined dj
		ON dj.warehouse_id = cb.warehouse_id
	   AND dj.district_id = cb.district_id
),
orders_base AS (
	SELECT
		o.O_W_ID AS warehouse_id,
		o.O_D_ID AS district_id,
		o.O_C_ID AS customer_id,
		o.O_ID AS order_id,
		COALESCE(o.O_CARRIER_ID, -1) AS carrier_id,
		CAST(o.O_OL_CNT AS BIGINT) AS order_line_count
	FROM dl.OORDER o
	WHERE o.O_ID IS NOT NULL
),
order_line_base AS (
	SELECT
		ol.OL_W_ID AS warehouse_id,
		ol.OL_D_ID AS district_id,
		ol.OL_O_ID AS order_id,
		ol.OL_NUMBER AS line_number,
		CAST(ol.OL_AMOUNT AS DOUBLE) AS line_amount,
		CAST(ol.OL_QUANTITY AS BIGINT) AS quantity
	FROM dl.ORDER_LINE ol
	WHERE ol.OL_NUMBER > 0
),
order_amounts AS (
	SELECT
		ob.warehouse_id,
		ob.district_id,
		ob.customer_id,
		ob.order_id,
		MAX(ob.carrier_id) AS carrier_id,
		SUM(olb.line_amount) AS order_amount,
		SUM(olb.quantity) AS ordered_quantity,
		COUNT(*) AS line_rows
	FROM orders_base ob
	JOIN order_line_base olb
		ON olb.warehouse_id = ob.warehouse_id
	   AND olb.district_id = ob.district_id
	   AND olb.order_id = ob.order_id
	GROUP BY
		ob.warehouse_id,
		ob.district_id,
		ob.customer_id,
		ob.order_id
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
history_rollup AS (
	SELECT
		hb.warehouse_id,
		hb.district_id,
		hb.customer_id,
		SUM(hb.history_amount) AS total_history_amount,
		SUM(CASE WHEN hb.history_data LIKE '%payment%' THEN 1 ELSE 0 END) AS payment_history_rows
	FROM history_base hb
	GROUP BY
		hb.warehouse_id,
		hb.district_id,
		hb.customer_id
),
customer_activity AS (
	SELECT
		ce.warehouse_id,
		ce.district_id,
		ce.customer_id,
		ce.credit_code,
		ce.warehouse_state,
		ce.warehouse_tax_band,
		ce.balance_amount,
		ce.ytd_payment,
		ce.payment_count,
		COALESCE(hr.total_history_amount, 0.0) AS total_history_amount,
		COALESCE(hr.payment_history_rows, 0) AS payment_history_rows,
		COALESCE(oa.order_amount, 0.0) AS order_amount,
		COALESCE(oa.ordered_quantity, 0) AS ordered_quantity,
		COALESCE(oa.line_rows, 0) AS line_rows,
		CASE
			WHEN oa.carrier_id = -1 THEN 'open'
			WHEN oa.order_amount >= ce.ytd_payment THEN 'large'
			ELSE 'closed'
		END AS activity_bucket
	FROM customer_enriched ce
	LEFT JOIN history_rollup hr
		ON hr.warehouse_id = ce.warehouse_id
	   AND hr.district_id = ce.district_id
	   AND hr.customer_id = ce.customer_id
	LEFT JOIN order_amounts oa
		ON oa.warehouse_id = ce.warehouse_id
	   AND oa.district_id = ce.district_id
	   AND oa.customer_id = ce.customer_id
),
rollup AS (
	SELECT
		ca.warehouse_id,
		ca.district_id,
		ca.warehouse_state,
		ca.warehouse_tax_band,
		ca.credit_code,
		ca.activity_bucket,
		COUNT(*) AS row_count,
		COUNT(*) FILTER (WHERE ca.credit_code = (SELECT good_credit FROM params)) AS good_credit_rows,
		COUNT(*) FILTER (WHERE ca.credit_code = (SELECT bad_credit FROM params)) AS bad_credit_rows,
		SUM(ca.balance_amount) FILTER (WHERE ca.balance_amount > (SELECT high_balance_cutoff FROM params)) AS high_balance_sum,
		SUM(ca.order_amount) FILTER (WHERE ca.activity_bucket IN ('open', 'large')) AS active_order_sum,
		SUM(ca.total_history_amount) FILTER (WHERE ca.payment_history_rows > 0) AS payment_history_sum,
		SUM(ca.ordered_quantity) AS ordered_quantity_sum,
		MIN(ca.balance_amount) AS min_balance,
		MAX(ca.balance_amount) AS max_balance
	FROM customer_activity ca
	GROUP BY
		ca.warehouse_id,
		ca.district_id,
		ca.warehouse_state,
		ca.warehouse_tax_band,
		ca.credit_code,
		ca.activity_bucket
	HAVING
		COUNT(*) FILTER (WHERE ca.credit_code IS NOT NULL) > 0
		AND (
			SUM(ca.order_amount) FILTER (WHERE ca.activity_bucket IN ('open', 'large')) IS NOT NULL
			OR SUM(ca.balance_amount) FILTER (WHERE ca.balance_amount <= (SELECT high_balance_cutoff FROM params)) IS NOT NULL
		)
),
final_projection AS (
	SELECT
		r.warehouse_id,
		r.district_id,
		r.warehouse_state,
		r.warehouse_tax_band,
		r.credit_code,
		r.activity_bucket,
		r.row_count,
		r.good_credit_rows,
		r.bad_credit_rows,
		COALESCE(r.high_balance_sum, (SELECT zero_amount FROM params)) AS high_balance_sum,
		COALESCE(r.active_order_sum, (SELECT zero_amount FROM params)) AS active_order_sum,
		COALESCE(r.payment_history_sum, (SELECT zero_amount FROM params)) AS payment_history_sum,
		r.ordered_quantity_sum,
		r.min_balance,
		r.max_balance
	FROM rollup r
)
SELECT
	warehouse_id,
	district_id,
	warehouse_state,
	warehouse_tax_band,
	credit_code,
	activity_bucket,
	row_count,
	good_credit_rows,
	bad_credit_rows,
	high_balance_sum,
	active_order_sum,
	payment_history_sum,
	ordered_quantity_sum,
	min_balance,
	max_balance
FROM final_projection
ORDER BY
	warehouse_id,
	district_id,
	credit_code,
	activity_bucket
