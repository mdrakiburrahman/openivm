-- {"operators": "DUCKLAKE,CTE,WINDOW,INNER_JOIN,OUTER_JOIN,UNION,AGGREGATE,DISTINCT,ORDER", "complexity": "high", "is_incremental": true, "has_nulls": true, "has_cast": true, "has_case": true, "tables": "WAREHOUSE,DISTRICT,CUSTOMER,ITEM,STOCK,OORDER,NEW_ORDER,ORDER_LINE,HISTORY", "ducklake": true, "openivm_verified": true}
WITH
parameters AS (
	SELECT
		DATE '2026-06-07' AS start_date,
		DATE '2026-06-15' AS end_date,
		8::INTEGER AS lookback_days,
		'healthy' AS ready_status,
		'degraded' AS review_status,
		'unknown' AS blocked_status,
		'orders_5' AS order_bucket,
		'payments_2' AS payment_bucket,
		'stock_0' AS stock_bucket
),
date_spine AS (
		SELECT DATE '2026-06-07' AS report_as_of_date
		UNION ALL SELECT DATE '2026-06-08' AS report_as_of_date
		UNION ALL SELECT DATE '2026-06-09' AS report_as_of_date
		UNION ALL SELECT DATE '2026-06-10' AS report_as_of_date
		UNION ALL SELECT DATE '2026-06-11' AS report_as_of_date
		UNION ALL SELECT DATE '2026-06-12' AS report_as_of_date
		UNION ALL SELECT DATE '2026-06-13' AS report_as_of_date
		UNION ALL SELECT DATE '2026-06-14' AS report_as_of_date
		UNION ALL SELECT DATE '2026-06-15' AS report_as_of_date
),
stg_warehouse AS (
	SELECT
		w.W_ID AS warehouse_id,
		LOWER(TRIM(w.W_NAME)) AS warehouse_name_norm,
		COALESCE(w.W_STATE, 'NA') AS warehouse_state,
		CAST(w.W_YTD AS DOUBLE) AS warehouse_ytd,
		CAST(w.W_TAX AS DOUBLE) AS warehouse_tax,
		ROW_NUMBER() OVER (
			PARTITION BY w.W_ID
			ORDER BY w.W_YTD DESC, w.W_NAME ASC
		) AS rn
	FROM dl.WAREHOUSE w
	WHERE w.W_YTD >= 0
),
latest_warehouse AS (
	SELECT
		warehouse_id,
		warehouse_name_norm,
		warehouse_state,
		warehouse_ytd,
		warehouse_tax
	FROM stg_warehouse
	WHERE rn = 1
),
stg_district AS (
	SELECT
		d.D_W_ID AS warehouse_id,
		d.D_ID AS district_id,
		LOWER(TRIM(d.D_NAME)) AS district_name_norm,
		CAST(d.D_YTD AS DOUBLE) AS district_ytd,
		CAST(d.D_TAX AS DOUBLE) AS district_tax,
		d.D_NEXT_O_ID AS next_order_id,
		ROW_NUMBER() OVER (
			PARTITION BY d.D_W_ID, d.D_ID
			ORDER BY d.D_NEXT_O_ID DESC, d.D_YTD DESC
		) AS rn
	FROM dl.DISTRICT d
	WHERE d.D_ID % 2 >= 0
),
latest_district AS (
	SELECT
		warehouse_id,
		district_id,
		district_name_norm,
		district_ytd,
		district_tax,
		next_order_id
	FROM stg_district
	WHERE rn = 1
),
stg_customer AS (
	SELECT
		c.C_W_ID AS warehouse_id,
		c.C_D_ID AS district_id,
		c.C_ID AS customer_id,
		LOWER(TRIM(c.C_LAST)) AS customer_last_norm,
		LOWER(TRIM(c.C_FIRST)) AS customer_first_norm,
		COALESCE(NULLIF(TRIM(c.C_CREDIT), ''), 'GC') AS credit_code,
		CAST(c.C_BALANCE AS DOUBLE) AS balance_amount,
		CAST(c.C_YTD_PAYMENT AS DOUBLE) AS ytd_payment,
		CAST(c.C_DISCOUNT AS DOUBLE) AS discount_rate,
		ROW_NUMBER() OVER (
			PARTITION BY c.C_W_ID, c.C_D_ID, c.C_ID
			ORDER BY c.C_SINCE DESC NULLS LAST, c.C_BALANCE DESC
		) AS rn
	FROM dl.CUSTOMER c
	WHERE c.C_BALANCE >= -475
),
latest_customer AS (
	SELECT
		warehouse_id,
		district_id,
		customer_id,
		customer_last_norm,
		customer_first_norm,
		credit_code,
		balance_amount,
		ytd_payment,
		discount_rate
	FROM stg_customer
	WHERE rn = 1
),
stg_item AS (
	SELECT
		i.I_ID AS item_id,
		LOWER(TRIM(i.I_NAME)) AS item_name_norm,
		CAST(i.I_PRICE AS DOUBLE) AS item_price,
		i.I_IM_ID AS image_id,
		ROW_NUMBER() OVER (
			PARTITION BY i.I_ID
			ORDER BY i.I_PRICE DESC, i.I_NAME ASC
		) AS rn
	FROM dl.ITEM i
	WHERE i.I_PRICE >= 13
),
latest_item AS (
	SELECT
		item_id,
		item_name_norm,
		item_price,
		image_id
	FROM stg_item
	WHERE rn = 1
),
stg_stock AS (
	SELECT
		s.S_W_ID AS warehouse_id,
		s.S_I_ID AS item_id,
		s.S_QUANTITY AS stock_quantity,
		CAST(s.S_YTD AS DOUBLE) AS stock_ytd,
		s.S_ORDER_CNT AS stock_order_count,
		s.S_REMOTE_CNT AS stock_remote_count,
		ROW_NUMBER() OVER (
			PARTITION BY s.S_W_ID, s.S_I_ID
			ORDER BY s.S_ORDER_CNT DESC, s.S_QUANTITY DESC
		) AS rn
	FROM dl.STOCK s
	WHERE s.S_QUANTITY >= 47
),
latest_stock AS (
	SELECT
		warehouse_id,
		item_id,
		stock_quantity,
		stock_ytd,
		stock_order_count,
		stock_remote_count
	FROM stg_stock
	WHERE rn = 1
),
target_customers AS (
	SELECT
		lw.warehouse_id,
		ld.district_id,
		lc.customer_id,
		lw.warehouse_name_norm,
		ld.district_name_norm,
		lc.customer_last_norm,
		lc.customer_first_norm,
		lc.credit_code,
		lc.balance_amount,
		lc.ytd_payment,
		lc.discount_rate,
		lw.warehouse_ytd,
		ld.district_ytd,
		lw.warehouse_tax,
		ld.district_tax
	FROM latest_warehouse lw
	INNER JOIN latest_district ld
		ON lw.warehouse_id = ld.warehouse_id
	INNER JOIN latest_customer lc
		ON ld.warehouse_id = lc.warehouse_id
		AND ld.district_id = lc.district_id
),
order_line_fact AS (
	SELECT
		ol.OL_W_ID AS warehouse_id,
		ol.OL_D_ID AS district_id,
		ol.OL_O_ID AS order_id,
		ol.OL_NUMBER AS line_number,
		ol.OL_I_ID AS item_id,
		CAST(ol.OL_AMOUNT AS DOUBLE) AS line_amount,
		CAST(ol.OL_QUANTITY AS DOUBLE) AS line_quantity,
		li.item_price,
		li.image_id,
		ls.stock_quantity,
		ls.stock_order_count
	FROM dl.ORDER_LINE ol
	INNER JOIN latest_item li
		ON ol.OL_I_ID = li.item_id
	LEFT JOIN latest_stock ls
		ON ol.OL_SUPPLY_W_ID = ls.warehouse_id
		AND ol.OL_I_ID = ls.item_id
	WHERE ol.OL_AMOUNT >= 0
),
raw_order_activity AS (
	SELECT
		ds.report_as_of_date,
		o.O_W_ID AS warehouse_id,
		o.O_D_ID AS district_id,
		o.O_ID AS order_id,
		o.O_C_ID AS customer_id,
		olf.item_id,
		olf.line_number,
		olf.line_amount,
		olf.line_quantity,
		olf.item_price,
		COALESCE(olf.stock_quantity, 0) AS stock_quantity,
		COALESCE(olf.stock_order_count, 0) AS stock_order_count,
		CASE
			WHEN no.NO_O_ID IS NOT NULL THEN 'degraded'
			WHEN o.O_CARRIER_ID IS NULL THEN 'unknown'
			ELSE 'healthy'
		END AS order_status
	FROM dl.OORDER o
	INNER JOIN order_line_fact olf
		ON o.O_W_ID = olf.warehouse_id
		AND o.O_D_ID = olf.district_id
		AND o.O_ID = olf.order_id
	LEFT JOIN dl.NEW_ORDER no
		ON o.O_W_ID = no.NO_W_ID
		AND o.O_D_ID = no.NO_D_ID
		AND o.O_ID = no.NO_O_ID
	INNER JOIN date_spine ds
		ON CAST(o.O_ENTRY_D AS DATE) BETWEEN ds.report_as_of_date - INTERVAL 8 DAY
		AND ds.report_as_of_date
),
ranked_order_activity AS (
	SELECT
		ra.*,
		ROW_NUMBER() OVER (
			PARTITION BY ra.report_as_of_date, ra.warehouse_id, ra.district_id, ra.order_id
			ORDER BY ra.line_amount DESC, ra.line_number ASC
		) AS rn
	FROM raw_order_activity ra
),
latest_order_activity AS (
	SELECT
		report_as_of_date,
		warehouse_id,
		district_id,
		order_id,
		customer_id,
		item_id,
		line_amount,
		line_quantity,
		item_price,
		stock_quantity,
		stock_order_count,
		order_status
	FROM ranked_order_activity
	WHERE rn = 1
),
history_activity AS (
	SELECT
		ds.report_as_of_date,
		h.H_C_W_ID AS warehouse_id,
		h.H_C_D_ID AS district_id,
		h.H_C_ID AS customer_id,
		COUNT(*) AS payment_events,
		SUM(CAST(h.H_AMOUNT AS DOUBLE)) AS payment_amount
	FROM dl.HISTORY h
	INNER JOIN date_spine ds
		ON CAST(h.H_DATE AS DATE) BETWEEN ds.report_as_of_date - INTERVAL 8 DAY
		AND ds.report_as_of_date
	GROUP BY
		ds.report_as_of_date,
		h.H_C_W_ID,
		h.H_C_D_ID,
		h.H_C_ID
),
eligible_customers AS (
	SELECT
		ds.report_as_of_date,
		tc.warehouse_id,
		tc.district_id,
		tc.customer_id,
		tc.warehouse_name_norm,
		tc.district_name_norm,
		tc.customer_last_norm,
		tc.customer_first_norm,
		tc.credit_code,
		tc.balance_amount,
		tc.ytd_payment,
		tc.discount_rate,
		tc.warehouse_ytd,
		tc.district_ytd,
		tc.warehouse_tax,
		tc.district_tax,
		COALESCE(ha.payment_events, 0) AS payment_events,
		COALESCE(ha.payment_amount, 0.0) AS payment_amount
	FROM target_customers tc
	CROSS JOIN date_spine ds
	LEFT JOIN history_activity ha
		ON ds.report_as_of_date = ha.report_as_of_date
		AND tc.warehouse_id = ha.warehouse_id
		AND tc.district_id = ha.district_id
		AND tc.customer_id = ha.customer_id
),
unpivoted AS (
	SELECT
		ec.report_as_of_date,
		ec.warehouse_id,
		ec.district_id,
		ec.customer_id,
		ec.credit_code,
		ec.warehouse_name_norm,
		ec.district_name_norm,
		ec.customer_last_norm,
		'customer_flow' AS metric_family,
		loa.order_status AS metric_status,
		loa.item_id,
		loa.stock_quantity,
		COUNT(DISTINCT loa.order_id) AS event_count,
		SUM(loa.line_amount * (1.0 - ec.discount_rate)) AS metric_amount,
		SUM(loa.line_quantity) AS metric_quantity
	FROM eligible_customers ec
	INNER JOIN latest_order_activity loa
		ON ec.report_as_of_date = loa.report_as_of_date
		AND ec.warehouse_id = loa.warehouse_id
		AND ec.district_id = loa.district_id
		AND ec.customer_id = loa.customer_id
	GROUP BY
		ec.report_as_of_date,
		ec.warehouse_id,
		ec.district_id,
		ec.customer_id,
		ec.credit_code,
		ec.warehouse_name_norm,
		ec.district_name_norm,
		ec.customer_last_norm,
		loa.order_status,
		loa.item_id,
		loa.stock_quantity
	UNION ALL
	SELECT
		ec.report_as_of_date,
		ec.warehouse_id,
		ec.district_id,
		ec.customer_id,
		ec.credit_code,
		ec.warehouse_name_norm,
		ec.district_name_norm,
		ec.customer_last_norm,
		'history_flow' AS metric_family,
		CASE
			WHEN ec.payment_amount > ec.balance_amount THEN 'healthy'
			WHEN ec.payment_events > 0 THEN 'degraded'
			ELSE 'unknown'
		END AS metric_status,
		CAST(NULL AS INTEGER) AS item_id,
		CAST(NULL AS INTEGER) AS stock_quantity,
		ec.payment_events AS event_count,
		ec.payment_amount AS metric_amount,
		ec.ytd_payment AS metric_quantity
	FROM eligible_customers ec
	UNION ALL
	SELECT
		ds.report_as_of_date,
		ls.warehouse_id,
		0 AS district_id,
		0 AS customer_id,
		'NA' AS credit_code,
		lw.warehouse_name_norm,
		'warehouse' AS district_name_norm,
		li.item_name_norm AS customer_last_norm,
		'stock_flow' AS metric_family,
		CASE
			WHEN ls.stock_quantity > 67 THEN 'healthy'
			WHEN ls.stock_quantity > 47 THEN 'degraded'
			ELSE 'unknown'
		END AS metric_status,
		ls.item_id,
		ls.stock_quantity,
		COUNT(*) AS event_count,
		SUM(CAST(ls.stock_quantity AS DOUBLE) * li.item_price) AS metric_amount,
		SUM(CAST(ls.stock_order_count + ls.stock_remote_count AS DOUBLE)) AS metric_quantity
	FROM latest_stock ls
	INNER JOIN latest_item li
		ON ls.item_id = li.item_id
	INNER JOIN latest_warehouse lw
		ON ls.warehouse_id = lw.warehouse_id
	CROSS JOIN date_spine ds
	GROUP BY
		ds.report_as_of_date,
		ls.warehouse_id,
		lw.warehouse_name_norm,
		li.item_name_norm,
		ls.item_id,
		ls.stock_quantity
),
agg AS (
	SELECT
		report_as_of_date,
		warehouse_id,
		district_id,
		credit_code,
		metric_family,
		metric_status,
		COUNT(DISTINCT customer_id) AS subject_count,
		COUNT(DISTINCT item_id) AS item_count,
		SUM(event_count) AS event_count,
		SUM(metric_amount) AS metric_amount,
		SUM(metric_quantity) AS metric_quantity,
		MIN(stock_quantity) AS min_stock_quantity,
		MAX(stock_quantity) AS max_stock_quantity
	FROM unpivoted
	GROUP BY
		report_as_of_date,
		warehouse_id,
		district_id,
		credit_code,
		metric_family,
		metric_status
),
agg_distinct_customers AS (
	SELECT
		report_as_of_date,
		warehouse_id,
		district_id,
		credit_code,
		metric_family,
		metric_status,
		COUNT(*) AS distinct_name_count,
		SUM(LENGTH(customer_last_norm)) AS name_length_sum
	FROM (
		SELECT DISTINCT
			report_as_of_date,
			warehouse_id,
			district_id,
			credit_code,
			metric_family,
			metric_status,
			customer_last_norm
		FROM unpivoted
		WHERE customer_last_norm IS NOT NULL
	) names
	GROUP BY
		report_as_of_date,
		warehouse_id,
		district_id,
		credit_code,
		metric_family,
		metric_status
),
final_enriched AS (
	SELECT
		a.report_as_of_date,
		a.warehouse_id,
		a.district_id,
		a.credit_code,
		a.metric_family,
		a.metric_status,
		a.subject_count,
		a.item_count,
		a.event_count,
		a.metric_amount,
		a.metric_quantity,
		a.min_stock_quantity,
		a.max_stock_quantity,
		COALESCE(adc.distinct_name_count, 0) AS distinct_name_count,
		COALESCE(adc.name_length_sum, 0) AS name_length_sum,
		lw.warehouse_state,
		lw.warehouse_tax,
		ld.district_tax
	FROM agg a
	LEFT JOIN agg_distinct_customers adc
		ON a.report_as_of_date = adc.report_as_of_date
		AND a.warehouse_id = adc.warehouse_id
		AND a.district_id = adc.district_id
		AND a.credit_code = adc.credit_code
		AND a.metric_family = adc.metric_family
		AND a.metric_status = adc.metric_status
	LEFT JOIN latest_warehouse lw
		ON a.warehouse_id = lw.warehouse_id
	LEFT JOIN latest_district ld
		ON a.warehouse_id = ld.warehouse_id
		AND a.district_id = ld.district_id
)
SELECT
	md5(
		concat(
			CAST(report_as_of_date AS VARCHAR),
			'-',
			CAST(warehouse_id AS VARCHAR),
			'-',
			CAST(district_id AS VARCHAR),
			'-',
			COALESCE(credit_code, 'NA'),
			'-',
			metric_family,
			'-',
			metric_status
		)
	) AS primary_key,
	report_as_of_date,
	warehouse_id,
	district_id,
	credit_code,
	metric_family,
	metric_status,
	CAST(subject_count AS BIGINT) AS subject_count,
	CAST(item_count AS BIGINT) AS item_count,
	CAST(event_count AS BIGINT) AS event_count,
	CAST(metric_amount AS DOUBLE) AS metric_amount,
	CAST(metric_quantity AS DOUBLE) AS metric_quantity,
	CAST(COALESCE(min_stock_quantity, 0) AS INTEGER) AS min_stock_quantity,
	CAST(COALESCE(max_stock_quantity, 0) AS INTEGER) AS max_stock_quantity,
	CAST(distinct_name_count AS BIGINT) AS distinct_name_count,
	CAST(name_length_sum AS BIGINT) AS name_length_sum,
	warehouse_state,
	CAST(COALESCE(warehouse_tax, 0.0) AS DOUBLE) AS warehouse_tax,
	CAST(COALESCE(district_tax, 0.0) AS DOUBLE) AS district_tax,
	CASE
		WHEN metric_amount >= 1700.0 THEN 'large'
		WHEN metric_amount >= 17.0 THEN 'medium'
		ELSE 'small'
	END AS metric_size
FROM final_enriched
ORDER BY
	report_as_of_date,
	warehouse_id,
	district_id,
	metric_family,
	metric_status;
