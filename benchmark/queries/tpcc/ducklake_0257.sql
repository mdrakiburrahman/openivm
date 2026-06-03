-- {"operators": "DUCKLAKE,CTE,NESTED_AGGREGATE,COUNT_DISTINCT,INNER_JOIN,LEFT_JOIN,HAVING,ORDER", "complexity": "high", "is_incremental": true, "has_nulls": true, "has_cast": true, "has_case": true, "tables": "WAREHOUSE,DISTRICT,CUSTOMER,OORDER,ORDER_LINE,ITEM,STOCK,HISTORY", "ducklake": true}
WITH
params AS (
	SELECT
		0.0::DOUBLE AS zero_amount,
		1::INTEGER AS min_group_rows,
		'GC' AS good_credit,
		'BC' AS bad_credit
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
		CAST(c.C_YTD_PAYMENT AS DOUBLE) AS ytd_payment
	FROM dl.CUSTOMER c
	WHERE c.C_ID > 0
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
		CAST(s.S_YTD AS DOUBLE) AS stock_ytd,
		CAST(s.S_ORDER_CNT AS BIGINT) AS stock_order_count
	FROM dl.STOCK s
	WHERE s.S_I_ID IS NOT NULL
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
		COALESCE(id.item_name, 'missing') AS item_name,
		COALESCE(id.item_price, (SELECT zero_amount FROM params)) AS item_price,
		COALESCE(id.is_original_item, 0) AS is_original_item,
		COALESCE(sd.stock_quantity, 0) AS stock_quantity,
		COALESCE(sd.stock_ytd, (SELECT zero_amount FROM params)) AS stock_ytd
	FROM line_base lb
	LEFT JOIN item_dim id
		ON id.item_id = lb.item_id
	LEFT JOIN stock_dim sd
		ON sd.warehouse_id = lb.warehouse_id
	   AND sd.item_id = lb.item_id
),
order_line_rollup AS (
	SELECT
		oh.warehouse_id,
		oh.district_id,
		oh.customer_id,
		oh.order_id,
		MAX(oh.carrier_id) AS carrier_id,
		COUNT(*) AS line_rows,
		COUNT(DISTINCT le.item_id) AS distinct_items,
		SUM(le.line_amount) AS order_amount,
		SUM(le.quantity) AS order_quantity,
		SUM(le.is_original_item) AS original_item_lines,
		MIN(le.stock_quantity) AS min_stock_quantity,
		MAX(le.stock_ytd) AS max_stock_ytd
	FROM order_header oh
	JOIN line_enriched le
		ON le.warehouse_id = oh.warehouse_id
	   AND le.district_id = oh.district_id
	   AND le.order_id = oh.order_id
	GROUP BY
		oh.warehouse_id,
		oh.district_id,
		oh.customer_id,
		oh.order_id
	HAVING COUNT(*) > 0
),
customer_order_rollup AS (
	SELECT
		olr.warehouse_id,
		olr.district_id,
		olr.customer_id,
		COUNT(*) AS order_rows,
		COUNT(DISTINCT olr.order_id) AS distinct_orders,
		SUM(olr.line_rows) AS total_line_rows,
		SUM(olr.distinct_items) AS distinct_items_per_order_sum,
		SUM(olr.order_amount) AS customer_order_amount,
		SUM(olr.order_quantity) AS customer_order_quantity,
		SUM(olr.original_item_lines) AS original_item_lines,
		MIN(olr.min_stock_quantity) AS min_customer_stock_quantity,
		MAX(olr.max_stock_ytd) AS max_customer_stock_ytd
	FROM order_line_rollup olr
	GROUP BY
		olr.warehouse_id,
		olr.district_id,
		olr.customer_id
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
customer_fact AS (
	SELECT
		cd.warehouse_id,
		cd.district_id,
		cd.customer_id,
		cd.credit_code,
		cd.balance_amount,
		cd.ytd_payment,
		dd.district_name,
		wd.warehouse_state,
		COALESCE(cor.order_rows, 0) AS order_rows,
		COALESCE(cor.distinct_orders, 0) AS distinct_orders,
		COALESCE(cor.total_line_rows, 0) AS total_line_rows,
		COALESCE(cor.distinct_items_per_order_sum, 0) AS distinct_items_per_order_sum,
		COALESCE(cor.customer_order_amount, 0.0) AS customer_order_amount,
		COALESCE(cor.customer_order_quantity, 0) AS customer_order_quantity,
		COALESCE(cor.original_item_lines, 0) AS original_item_lines,
		COALESCE(cor.min_customer_stock_quantity, 0) AS min_customer_stock_quantity,
		COALESCE(cor.max_customer_stock_ytd, 0.0) AS max_customer_stock_ytd,
		COALESCE(hr.history_amount, 0.0) AS history_amount,
		COALESCE(hr.history_rows, 0) AS history_rows
	FROM customer_dim cd
	JOIN district_dim dd
		ON dd.warehouse_id = cd.warehouse_id
	   AND dd.district_id = cd.district_id
	JOIN warehouse_dim wd
		ON wd.warehouse_id = cd.warehouse_id
	LEFT JOIN customer_order_rollup cor
		ON cor.warehouse_id = cd.warehouse_id
	   AND cor.district_id = cd.district_id
	   AND cor.customer_id = cd.customer_id
	LEFT JOIN history_rollup hr
		ON hr.warehouse_id = cd.warehouse_id
	   AND hr.district_id = cd.district_id
	   AND hr.customer_id = cd.customer_id
),
district_customer_rollup AS (
	SELECT
		cf.warehouse_id,
		cf.district_id,
		cf.warehouse_state,
		cf.district_name,
		cf.credit_code,
		COUNT(*) AS customer_rows,
		COUNT(DISTINCT cf.customer_id) AS distinct_customers,
		SUM(cf.balance_amount) AS balance_sum,
		SUM(cf.ytd_payment) AS payment_sum,
		SUM(cf.customer_order_amount) AS order_amount_sum,
		SUM(cf.customer_order_quantity) AS order_quantity_sum,
		SUM(cf.original_item_lines) AS original_item_line_sum,
		SUM(cf.history_amount) AS history_amount_sum,
		SUM(cf.history_rows) AS history_row_sum,
		MIN(cf.min_customer_stock_quantity) AS district_min_stock_quantity,
		MAX(cf.max_customer_stock_ytd) AS district_max_stock_ytd
	FROM customer_fact cf
	GROUP BY
		cf.warehouse_id,
		cf.district_id,
		cf.warehouse_state,
		cf.district_name,
		cf.credit_code
),
warehouse_credit_rollup AS (
	SELECT
		dcr.warehouse_id,
		dcr.warehouse_state,
		dcr.credit_code,
		COUNT(*) AS district_rows,
		SUM(dcr.customer_rows) AS customer_rows,
		SUM(dcr.distinct_customers) AS distinct_customers_sum,
		SUM(dcr.balance_sum) AS balance_sum,
		SUM(dcr.payment_sum) AS payment_sum,
		SUM(dcr.order_amount_sum) AS order_amount_sum,
		SUM(dcr.order_quantity_sum) AS order_quantity_sum,
		SUM(dcr.original_item_line_sum) AS original_item_line_sum,
		SUM(dcr.history_amount_sum) AS history_amount_sum,
		SUM(dcr.history_row_sum) AS history_row_sum,
		MIN(dcr.district_min_stock_quantity) AS min_stock_quantity,
		MAX(dcr.district_max_stock_ytd) AS max_stock_ytd
	FROM district_customer_rollup dcr
	GROUP BY
		dcr.warehouse_id,
		dcr.warehouse_state,
		dcr.credit_code
	HAVING SUM(dcr.customer_rows) >= (SELECT min_group_rows FROM params)
),
final_rows AS (
	SELECT
		wcr.warehouse_id,
		wcr.warehouse_state,
		wcr.credit_code,
		wcr.district_rows,
		wcr.customer_rows,
		wcr.distinct_customers_sum,
		wcr.balance_sum,
		wcr.payment_sum,
		wcr.order_amount_sum,
		wcr.order_quantity_sum,
		wcr.original_item_line_sum,
		wcr.history_amount_sum,
		wcr.history_row_sum,
		wcr.min_stock_quantity,
		wcr.max_stock_ytd
	FROM warehouse_credit_rollup wcr
)
SELECT
	warehouse_id,
	warehouse_state,
	credit_code,
	district_rows,
	customer_rows,
	distinct_customers_sum,
	balance_sum,
	payment_sum,
	order_amount_sum,
	order_quantity_sum,
	original_item_line_sum,
	history_amount_sum,
	history_row_sum,
	min_stock_quantity,
	max_stock_ytd
FROM final_rows
ORDER BY
	warehouse_id,
	credit_code
