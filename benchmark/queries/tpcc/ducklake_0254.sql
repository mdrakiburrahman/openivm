-- {"operators": "DUCKLAKE,CTE,SEMI_ANTI,EXISTS,NOT_EXISTS,UNION,AGGREGATE,HAVING,ORDER", "complexity": "high", "is_incremental": true, "has_nulls": true, "has_cast": true, "has_case": true, "tables": "WAREHOUSE,DISTRICT,CUSTOMER,OORDER,NEW_ORDER,ORDER_LINE,HISTORY,ITEM,STOCK", "ducklake": true}
WITH
params AS (
	SELECT
		DATE '2026-06-01' AS start_date,
		DATE '2026-06-30' AS end_date,
		0.05::DOUBLE AS min_tax,
		10::INTEGER AS low_stock_cutoff,
		'GC' AS good_credit,
		'BC' AS bad_credit,
		'exists' AS exists_branch,
		'anti' AS anti_branch
),
warehouse_scope AS (
	SELECT
		w.W_ID AS warehouse_id,
		LOWER(TRIM(w.W_NAME)) AS warehouse_name,
		COALESCE(w.W_STATE, 'NA') AS warehouse_state,
		CAST(w.W_TAX AS DOUBLE) AS warehouse_tax,
		CAST(w.W_YTD AS DOUBLE) AS warehouse_ytd
	FROM dl.WAREHOUSE w
	WHERE w.W_ID IS NOT NULL
),
warehouse_rank_inputs AS (
	SELECT
		ws.warehouse_id,
		ws.warehouse_name,
		ws.warehouse_state,
		ws.warehouse_tax,
		ws.warehouse_ytd,
		CASE
			WHEN ws.warehouse_tax >= (SELECT min_tax FROM params) THEN 'taxed'
			ELSE 'standard'
		END AS tax_band
	FROM warehouse_scope ws
),
district_scope AS (
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
district_enriched AS (
	SELECT
		ds.warehouse_id,
		ds.district_id,
		ds.district_name,
		ds.district_tax,
		ds.district_ytd,
		ds.next_order_id,
		wri.warehouse_state,
		wri.tax_band
	FROM district_scope ds
	JOIN warehouse_rank_inputs wri
		ON wri.warehouse_id = ds.warehouse_id
),
customer_seed AS (
	SELECT
		c.C_W_ID AS warehouse_id,
		c.C_D_ID AS district_id,
		c.C_ID AS customer_id,
		COALESCE(NULLIF(TRIM(c.C_LAST), ''), 'unknown') AS customer_last,
		COALESCE(NULLIF(TRIM(c.C_FIRST), ''), 'unknown') AS customer_first,
		COALESCE(c.C_CREDIT, (SELECT bad_credit FROM params)) AS credit_code,
		CAST(c.C_BALANCE AS DOUBLE) AS balance_amount,
		CAST(c.C_YTD_PAYMENT AS DOUBLE) AS ytd_payment,
		CAST(c.C_PAYMENT_CNT AS BIGINT) AS payment_count,
		CAST(c.C_DELIVERY_CNT AS BIGINT) AS delivery_count
	FROM dl.CUSTOMER c
	WHERE c.C_ID > 0
),
customer_joined AS (
	SELECT
		cs.warehouse_id,
		cs.district_id,
		cs.customer_id,
		cs.customer_last,
		cs.customer_first,
		cs.credit_code,
		cs.balance_amount,
		cs.ytd_payment,
		cs.payment_count,
		cs.delivery_count,
		de.district_name,
		de.district_tax,
		de.warehouse_state,
		de.tax_band
	FROM customer_seed cs
	JOIN district_enriched de
		ON de.warehouse_id = cs.warehouse_id
	   AND de.district_id = cs.district_id
),
customer_flags AS (
	SELECT
		cj.*,
		CASE
			WHEN cj.credit_code = (SELECT good_credit FROM params) THEN 'good'
			WHEN cj.balance_amount < 0 THEN 'negative'
			ELSE 'review'
		END AS customer_segment,
		CASE
			WHEN cj.payment_count = 0 THEN 0.0
			ELSE cj.ytd_payment / NULLIF(cj.payment_count, 0)
		END AS avg_payment_amount
	FROM customer_joined cj
),
customer_left AS (
	SELECT
		cf.warehouse_id,
		cf.district_id,
		cf.customer_id,
		cf.customer_last,
		cf.customer_first,
		cf.credit_code,
		cf.customer_segment,
		cf.balance_amount,
		cf.ytd_payment,
		cf.avg_payment_amount,
		cf.delivery_count,
		cf.district_name,
		cf.warehouse_state,
		cf.tax_band
	FROM customer_flags cf
	WHERE cf.customer_segment IN ('good', 'negative', 'review')
),
orders_base AS (
	SELECT
		o.O_W_ID AS warehouse_id,
		o.O_D_ID AS district_id,
		o.O_C_ID AS customer_id,
		o.O_ID AS order_id,
		COALESCE(o.O_CARRIER_ID, -1) AS carrier_id,
		CAST(o.O_OL_CNT AS BIGINT) AS order_line_count,
		CAST(o.O_ALL_LOCAL AS BIGINT) AS all_local_flag
	FROM dl.OORDER o
	WHERE o.O_ID IS NOT NULL
),
new_order_keys AS (
	SELECT
		no.NO_W_ID AS warehouse_id,
		no.NO_D_ID AS district_id,
		no.NO_O_ID AS order_id,
		1 AS is_new_order
	FROM dl.NEW_ORDER no
),
order_line_base AS (
	SELECT
		ol.OL_W_ID AS warehouse_id,
		ol.OL_D_ID AS district_id,
		ol.OL_O_ID AS order_id,
		ol.OL_NUMBER AS line_number,
		ol.OL_I_ID AS item_id,
		CAST(ol.OL_QUANTITY AS BIGINT) AS quantity,
		CAST(ol.OL_AMOUNT AS DOUBLE) AS line_amount
	FROM dl.ORDER_LINE ol
	WHERE ol.OL_NUMBER > 0
),
item_scope AS (
	SELECT
		i.I_ID AS item_id,
		LOWER(TRIM(i.I_NAME)) AS item_name,
		CAST(i.I_PRICE AS DOUBLE) AS item_price,
		CASE
			WHEN LOWER(COALESCE(i.I_DATA, '')) LIKE '%original%' THEN 1
			ELSE 0
		END AS is_original_item
	FROM dl.ITEM i
	WHERE i.I_ID IS NOT NULL
),
stock_scope AS (
	SELECT
		s.S_W_ID AS warehouse_id,
		s.S_I_ID AS item_id,
		CAST(s.S_QUANTITY AS BIGINT) AS stock_quantity,
		CAST(s.S_YTD AS DOUBLE) AS stock_ytd,
		CAST(s.S_ORDER_CNT AS BIGINT) AS stock_order_count,
		CAST(s.S_REMOTE_CNT AS BIGINT) AS stock_remote_count
	FROM dl.STOCK s
	WHERE s.S_I_ID IS NOT NULL
),
line_enriched AS (
	SELECT
		olb.warehouse_id,
		olb.district_id,
		olb.order_id,
		olb.line_number,
		olb.item_id,
		olb.quantity,
		olb.line_amount,
		COALESCE(its.item_price, 0.0) AS item_price,
		COALESCE(its.is_original_item, 0) AS is_original_item,
		COALESCE(ss.stock_quantity, 0) AS stock_quantity,
		COALESCE(ss.stock_remote_count, 0) AS stock_remote_count
	FROM order_line_base olb
	LEFT JOIN item_scope its
		ON its.item_id = olb.item_id
	LEFT JOIN stock_scope ss
		ON ss.warehouse_id = olb.warehouse_id
	   AND ss.item_id = olb.item_id
),
order_rollup AS (
	SELECT
		ob.warehouse_id,
		ob.district_id,
		ob.customer_id,
		ob.order_id,
		MAX(ob.carrier_id) AS carrier_id,
		MAX(ob.order_line_count) AS expected_line_count,
		SUM(le.quantity) AS ordered_quantity,
		SUM(le.line_amount) AS ordered_amount,
		SUM(CASE WHEN le.stock_quantity < (SELECT low_stock_cutoff FROM params) THEN 1 ELSE 0 END) AS low_stock_lines,
		SUM(CASE WHEN le.is_original_item = 1 THEN 1 ELSE 0 END) AS original_item_lines,
		SUM(CASE WHEN nok.is_new_order = 1 THEN 1 ELSE 0 END) AS open_order_lines
	FROM orders_base ob
	JOIN line_enriched le
		ON le.warehouse_id = ob.warehouse_id
	   AND le.district_id = ob.district_id
	   AND le.order_id = ob.order_id
	LEFT JOIN new_order_keys nok
		ON nok.warehouse_id = ob.warehouse_id
	   AND nok.district_id = ob.district_id
	   AND nok.order_id = ob.order_id
	GROUP BY
		ob.warehouse_id,
		ob.district_id,
		ob.customer_id,
		ob.order_id
),
right_order_probe AS (
	SELECT
		orr.warehouse_id,
		orr.district_id,
		orr.customer_id,
		orr.order_id,
		orr.ordered_amount,
		orr.ordered_quantity,
		orr.low_stock_lines,
		orr.original_item_lines,
		orr.open_order_lines,
		CASE
			WHEN orr.open_order_lines > 0 THEN 'open'
			WHEN orr.low_stock_lines > 0 THEN 'low_stock'
			ELSE 'closed'
		END AS order_status
	FROM order_rollup orr
	WHERE orr.ordered_amount > 0
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
		SUM(hb.history_amount) AS history_amount,
		COUNT(*) AS history_rows,
		SUM(CASE WHEN hb.history_data LIKE '%adjust%' THEN 1 ELSE 0 END) AS adjusted_rows
	FROM history_base hb
	GROUP BY
		hb.warehouse_id,
		hb.district_id,
		hb.customer_id
),
exists_customers AS (
	SELECT
		cl.warehouse_id,
		cl.district_id,
		cl.customer_id,
		cl.customer_last,
		cl.credit_code,
		cl.customer_segment,
		cl.balance_amount,
		cl.ytd_payment,
		cl.avg_payment_amount,
		cl.delivery_count,
		cl.warehouse_state,
		cl.tax_band,
		COALESCE(hr.history_amount, 0.0) AS history_amount,
		COALESCE(hr.history_rows, 0) AS history_rows,
		(SELECT exists_branch FROM params) AS branch_name
	FROM customer_left cl
	SEMI JOIN right_order_probe rop
		ON rop.warehouse_id = cl.warehouse_id
	   AND rop.district_id = cl.district_id
	   AND rop.customer_id = cl.customer_id
	   AND (
	   	rop.order_status IN ('open', 'low_stock')
	   	OR rop.ordered_amount > cl.avg_payment_amount
	   )
	LEFT JOIN history_rollup hr
		ON hr.warehouse_id = cl.warehouse_id
	   AND hr.district_id = cl.district_id
	   AND hr.customer_id = cl.customer_id
),
anti_customers AS (
	SELECT
		cl.warehouse_id,
		cl.district_id,
		cl.customer_id,
		cl.customer_last,
		cl.credit_code,
		cl.customer_segment,
		cl.balance_amount,
		cl.ytd_payment,
		cl.avg_payment_amount,
		cl.delivery_count,
		cl.warehouse_state,
		cl.tax_band,
		COALESCE(hr.history_amount, 0.0) AS history_amount,
		COALESCE(hr.history_rows, 0) AS history_rows,
		(SELECT anti_branch FROM params) AS branch_name
	FROM customer_left cl
	ANTI JOIN right_order_probe rop
		ON rop.warehouse_id = cl.warehouse_id
	   AND rop.district_id = cl.district_id
	   AND rop.customer_id = cl.customer_id
	   AND rop.order_status = 'open'
	   AND rop.ordered_amount >= 0
	LEFT JOIN history_rollup hr
		ON hr.warehouse_id = cl.warehouse_id
	   AND hr.district_id = cl.district_id
	   AND hr.customer_id = cl.customer_id
),
semantic_union AS (
	SELECT * FROM exists_customers
	UNION ALL
	SELECT * FROM anti_customers
),
segment_rollup AS (
	SELECT
		su.warehouse_id,
		su.district_id,
		su.warehouse_state,
		su.tax_band,
		su.credit_code,
		su.customer_segment,
		su.branch_name,
		COUNT(*) AS customer_rows,
		COUNT(DISTINCT su.customer_id) AS distinct_customers,
		SUM(su.balance_amount) AS balance_sum,
		SUM(su.ytd_payment) AS payment_sum,
		SUM(su.history_amount) AS history_sum,
		SUM(CASE WHEN su.history_rows > 0 THEN 1 ELSE 0 END) AS customers_with_history,
		MIN(su.avg_payment_amount) AS min_avg_payment,
		MAX(su.avg_payment_amount) AS max_avg_payment
	FROM semantic_union su
	GROUP BY
		su.warehouse_id,
		su.district_id,
		su.warehouse_state,
		su.tax_band,
		su.credit_code,
		su.customer_segment,
		su.branch_name
	HAVING
		COUNT(*) > 0
		AND (
			SUM(su.balance_amount) IS NOT NULL
			OR SUM(su.history_amount) IS NOT NULL
		)
),
final_named AS (
	SELECT
		sr.warehouse_id,
		sr.district_id,
		sr.warehouse_state,
		sr.tax_band,
		sr.credit_code,
		sr.customer_segment,
		sr.branch_name,
		sr.customer_rows,
		sr.distinct_customers,
		sr.balance_sum,
		sr.payment_sum,
		sr.history_sum,
		sr.customers_with_history,
		sr.min_avg_payment,
		sr.max_avg_payment,
		CASE
			WHEN sr.branch_name = (SELECT exists_branch FROM params) THEN sr.customer_rows
			ELSE -sr.customer_rows
		END AS branch_signed_rows
	FROM segment_rollup sr
)
SELECT
	warehouse_id,
	district_id,
	warehouse_state,
	tax_band,
	credit_code,
	customer_segment,
	branch_name,
	customer_rows,
	distinct_customers,
	balance_sum,
	payment_sum,
	history_sum,
	customers_with_history,
	min_avg_payment,
	max_avg_payment,
	branch_signed_rows
FROM final_named
ORDER BY
	warehouse_id,
	district_id,
	credit_code,
	customer_segment,
	branch_name
