-- {"operators": "DUCKLAKE,CTE,INNER_JOIN,LEFT_JOIN,AGGREGATE,TOP_K,LIMIT,OFFSET,ORDER", "complexity": "high", "is_incremental": true, "has_nulls": true, "has_cast": true, "has_case": true, "tables": "WAREHOUSE,DISTRICT,CUSTOMER,OORDER,ORDER_LINE,NEW_ORDER,STOCK,ITEM", "ducklake": true}
WITH
params AS (
	SELECT
		0.0::DOUBLE AS zero_amount,
		10::INTEGER AS low_stock_cutoff,
		'GC' AS good_credit,
		'BC' AS bad_credit
),
warehouse_dim AS (
	SELECT
		w.W_ID AS warehouse_id,
		COALESCE(w.W_STATE, 'NA') AS warehouse_state,
		LOWER(TRIM(w.W_NAME)) AS warehouse_name,
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
customer_dim AS (
	SELECT
		c.C_W_ID AS warehouse_id,
		c.C_D_ID AS district_id,
		c.C_ID AS customer_id,
		COALESCE(c.C_CREDIT, (SELECT bad_credit FROM params)) AS credit_code,
		CAST(c.C_BALANCE AS DOUBLE) AS balance_amount,
		CAST(c.C_YTD_PAYMENT AS DOUBLE) AS ytd_payment,
		CAST(c.C_PAYMENT_CNT AS BIGINT) AS payment_count
	FROM dl.CUSTOMER c
	WHERE c.C_ID > 0
),
order_header AS (
	SELECT
		o.O_W_ID AS warehouse_id,
		o.O_D_ID AS district_id,
		o.O_C_ID AS customer_id,
		o.O_ID AS order_id,
		COALESCE(o.O_CARRIER_ID, -1) AS carrier_id,
		CAST(o.O_OL_CNT AS BIGINT) AS expected_lines
	FROM dl.OORDER o
	WHERE o.O_ID IS NOT NULL
),
new_order_dim AS (
	SELECT
		no.NO_W_ID AS warehouse_id,
		no.NO_D_ID AS district_id,
		no.NO_O_ID AS order_id,
		1 AS is_new_order
	FROM dl.NEW_ORDER no
),
item_dim AS (
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
stock_dim AS (
	SELECT
		s.S_W_ID AS warehouse_id,
		s.S_I_ID AS item_id,
		CAST(s.S_QUANTITY AS BIGINT) AS stock_quantity,
		CAST(s.S_ORDER_CNT AS BIGINT) AS stock_order_count,
		CAST(s.S_REMOTE_CNT AS BIGINT) AS stock_remote_count
	FROM dl.STOCK s
	WHERE s.S_I_ID IS NOT NULL
),
line_base AS (
	SELECT
		ol.OL_W_ID AS warehouse_id,
		ol.OL_D_ID AS district_id,
		ol.OL_O_ID AS order_id,
		ol.OL_NUMBER AS line_number,
		ol.OL_I_ID AS item_id,
		CAST(ol.OL_AMOUNT AS DOUBLE) AS line_amount,
		CAST(ol.OL_QUANTITY AS BIGINT) AS quantity
	FROM dl.ORDER_LINE ol
	WHERE ol.OL_NUMBER > 0
),
line_enriched AS (
	SELECT
		lb.warehouse_id,
		lb.district_id,
		lb.order_id,
		lb.line_number,
		lb.item_id,
		lb.line_amount,
		lb.quantity,
		COALESCE(id.item_price, (SELECT zero_amount FROM params)) AS item_price,
		COALESCE(id.is_original_item, 0) AS is_original_item,
		COALESCE(sd.stock_quantity, 0) AS stock_quantity,
		COALESCE(sd.stock_remote_count, 0) AS stock_remote_count
	FROM line_base lb
	LEFT JOIN item_dim id
		ON id.item_id = lb.item_id
	LEFT JOIN stock_dim sd
		ON sd.warehouse_id = lb.warehouse_id
	   AND sd.item_id = lb.item_id
),
order_fact AS (
	SELECT
		oh.warehouse_id,
		oh.district_id,
		oh.customer_id,
		oh.order_id,
		oh.carrier_id,
		oh.expected_lines,
		COALESCE(nod.is_new_order, 0) AS is_new_order,
		SUM(le.line_amount) AS order_amount,
		SUM(le.quantity) AS ordered_quantity,
		SUM(CASE WHEN le.stock_quantity < (SELECT low_stock_cutoff FROM params) THEN 1 ELSE 0 END) AS low_stock_lines,
		SUM(le.is_original_item) AS original_item_lines,
		SUM(le.stock_remote_count) AS remote_stock_rows
	FROM order_header oh
	JOIN line_enriched le
		ON le.warehouse_id = oh.warehouse_id
	   AND le.district_id = oh.district_id
	   AND le.order_id = oh.order_id
	LEFT JOIN new_order_dim nod
		ON nod.warehouse_id = oh.warehouse_id
	   AND nod.district_id = oh.district_id
	   AND nod.order_id = oh.order_id
	GROUP BY
		oh.warehouse_id,
		oh.district_id,
		oh.customer_id,
		oh.order_id,
		oh.carrier_id,
		oh.expected_lines,
		nod.is_new_order
),
customer_activity AS (
	SELECT
		cd.warehouse_id,
		cd.district_id,
		cd.customer_id,
		cd.credit_code,
		cd.balance_amount,
		cd.ytd_payment,
		cd.payment_count,
		dd.district_name,
		wd.warehouse_state,
		wd.warehouse_tax,
		COALESCE(ofa.order_amount, 0.0) AS order_amount,
		COALESCE(ofa.ordered_quantity, 0) AS ordered_quantity,
		COALESCE(ofa.low_stock_lines, 0) AS low_stock_lines,
		COALESCE(ofa.original_item_lines, 0) AS original_item_lines,
		COALESCE(ofa.remote_stock_rows, 0) AS remote_stock_rows,
		COALESCE(ofa.is_new_order, 0) AS is_new_order
	FROM customer_dim cd
	JOIN district_dim dd
		ON dd.warehouse_id = cd.warehouse_id
	   AND dd.district_id = cd.district_id
	JOIN warehouse_dim wd
		ON wd.warehouse_id = cd.warehouse_id
	LEFT JOIN order_fact ofa
		ON ofa.warehouse_id = cd.warehouse_id
	   AND ofa.district_id = cd.district_id
	   AND ofa.customer_id = cd.customer_id
),
scored_activity AS (
	SELECT
		ca.*,
		CASE
			WHEN ca.credit_code = (SELECT good_credit FROM params) THEN 'good'
			WHEN ca.balance_amount < 0 THEN 'negative'
			ELSE 'review'
		END AS credit_bucket,
		(ca.order_amount + ca.ytd_payment + ca.low_stock_lines * 10.0 + ca.original_item_lines * 5.0) AS risk_score
	FROM customer_activity ca
),
district_rollup AS (
	SELECT
		sa.warehouse_id,
		sa.district_id,
		sa.warehouse_state,
		sa.district_name,
		sa.credit_bucket,
		COUNT(*) AS customer_event_rows,
		SUM(sa.order_amount) AS order_amount_sum,
		SUM(sa.ytd_payment) AS payment_sum,
		SUM(sa.ordered_quantity) AS ordered_quantity_sum,
		SUM(sa.low_stock_lines) AS low_stock_line_sum,
		SUM(sa.original_item_lines) AS original_item_line_sum,
		SUM(sa.remote_stock_rows) AS remote_stock_row_sum,
		SUM(sa.is_new_order) AS new_order_row_sum,
		MAX(sa.risk_score) AS max_risk_score,
		MIN(sa.risk_score) AS min_risk_score
	FROM scored_activity sa
	GROUP BY
		sa.warehouse_id,
		sa.district_id,
		sa.warehouse_state,
		sa.district_name,
		sa.credit_bucket
),
rank_ready AS (
	SELECT
		dr.warehouse_id,
		dr.district_id,
		dr.warehouse_state,
		dr.district_name,
		dr.credit_bucket,
		dr.customer_event_rows,
		dr.order_amount_sum,
		dr.payment_sum,
		dr.ordered_quantity_sum,
		dr.low_stock_line_sum,
		dr.original_item_line_sum,
		dr.remote_stock_row_sum,
		dr.new_order_row_sum,
		dr.max_risk_score,
		dr.min_risk_score,
		(dr.order_amount_sum + dr.payment_sum + dr.low_stock_line_sum * 10.0 + dr.new_order_row_sum * 2.0) AS sort_metric,
		(dr.warehouse_id * 100000 + dr.district_id * 100 + CASE WHEN dr.credit_bucket = 'good' THEN 1 ELSE 2 END) AS stable_rank_key
	FROM district_rollup dr
	WHERE dr.customer_event_rows > 0
)
SELECT
	warehouse_id,
	district_id,
	warehouse_state,
	district_name,
	credit_bucket,
	customer_event_rows,
	order_amount_sum,
	payment_sum,
	ordered_quantity_sum,
	low_stock_line_sum,
	original_item_line_sum,
	remote_stock_row_sum,
	new_order_row_sum,
	max_risk_score,
	min_risk_score,
	sort_metric,
	stable_rank_key
FROM rank_ready
ORDER BY
	sort_metric DESC,
	stable_rank_key ASC
LIMIT 25 OFFSET 3
