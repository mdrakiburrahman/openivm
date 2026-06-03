-- {"operators": "DUCKLAKE,CTE,WINDOW,FILTER,INNER_JOIN,LEFT_JOIN,PROJECTION,ORDER", "complexity": "high", "is_incremental": true, "has_nulls": true, "has_cast": true, "has_case": true, "tables": "WAREHOUSE,DISTRICT,CUSTOMER,OORDER,ORDER_LINE,HISTORY", "ducklake": true}
WITH
params AS (
	SELECT
		0.0::DOUBLE AS zero_amount,
		3::INTEGER AS top_orders_per_customer,
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
		COALESCE(NULLIF(TRIM(c.C_LAST), ''), 'unknown') AS customer_last,
		CAST(c.C_BALANCE AS DOUBLE) AS balance_amount,
		CAST(c.C_YTD_PAYMENT AS DOUBLE) AS ytd_payment
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
		CAST(o.O_OL_CNT AS BIGINT) AS order_line_count
	FROM dl.OORDER o
	WHERE o.O_ID IS NOT NULL
),
line_base AS (
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
order_rollup AS (
	SELECT
		oh.warehouse_id,
		oh.district_id,
		oh.customer_id,
		oh.order_id,
		MAX(oh.carrier_id) AS carrier_id,
		MAX(oh.order_line_count) AS expected_line_count,
		SUM(lb.line_amount) AS order_amount,
		SUM(lb.quantity) AS ordered_quantity,
		COUNT(*) AS actual_line_count
	FROM order_header oh
	JOIN line_base lb
		ON lb.warehouse_id = oh.warehouse_id
	   AND lb.district_id = oh.district_id
	   AND lb.order_id = oh.order_id
	GROUP BY
		oh.warehouse_id,
		oh.district_id,
		oh.customer_id,
		oh.order_id
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
order_customer AS (
	SELECT
		orr.warehouse_id,
		orr.district_id,
		orr.customer_id,
		orr.order_id,
		orr.carrier_id,
		orr.expected_line_count,
		orr.order_amount,
		orr.ordered_quantity,
		orr.actual_line_count,
		cd.credit_code,
		cd.customer_last,
		cd.balance_amount,
		cd.ytd_payment,
		dd.district_name,
		wd.warehouse_state,
		COALESCE(hr.history_amount, (SELECT zero_amount FROM params)) AS history_amount,
		COALESCE(hr.history_rows, 0) AS history_rows
	FROM order_rollup orr
	JOIN customer_dim cd
		ON cd.warehouse_id = orr.warehouse_id
	   AND cd.district_id = orr.district_id
	   AND cd.customer_id = orr.customer_id
	JOIN district_dim dd
		ON dd.warehouse_id = orr.warehouse_id
	   AND dd.district_id = orr.district_id
	JOIN warehouse_dim wd
		ON wd.warehouse_id = orr.warehouse_id
	LEFT JOIN history_rollup hr
		ON hr.warehouse_id = orr.warehouse_id
	   AND hr.district_id = orr.district_id
	   AND hr.customer_id = orr.customer_id
),
window_input AS (
	SELECT
		oc.*,
		CASE
			WHEN oc.credit_code = (SELECT good_credit FROM params) THEN 'good'
			WHEN oc.balance_amount < 0 THEN 'negative'
			ELSE 'review'
		END AS customer_bucket,
		(oc.order_amount + oc.history_amount + oc.ordered_quantity) AS activity_score
	FROM order_customer oc
),
windowed AS (
	SELECT
		wi.warehouse_id,
		wi.district_id,
		wi.customer_id,
		wi.order_id,
		wi.credit_code,
		wi.customer_bucket,
		wi.customer_last,
		wi.district_name,
		wi.warehouse_state,
		wi.order_amount,
		wi.ordered_quantity,
		wi.history_amount,
		wi.history_rows,
		wi.activity_score,
		ROW_NUMBER() OVER (
			PARTITION BY wi.warehouse_id, wi.district_id, wi.customer_id
			ORDER BY wi.activity_score DESC, wi.order_id ASC
		) AS customer_order_rank,
		SUM(wi.order_amount) OVER (
			PARTITION BY wi.warehouse_id, wi.district_id, wi.customer_id
			ORDER BY wi.order_id ASC
			ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
		) AS customer_running_order_amount,
		LAG(wi.order_amount) OVER (
			PARTITION BY wi.warehouse_id, wi.district_id
			ORDER BY wi.customer_id ASC, wi.order_id ASC
		) AS district_prev_order_amount,
		MAX(wi.activity_score) OVER (
			PARTITION BY wi.warehouse_id, wi.customer_bucket
		) AS bucket_max_activity_score
	FROM window_input wi
),
filtered_windowed AS (
	SELECT
		w.*,
		COALESCE(w.district_prev_order_amount, (SELECT zero_amount FROM params)) AS prev_order_amount,
		w.activity_score - COALESCE(w.district_prev_order_amount, (SELECT zero_amount FROM params)) AS score_gap
	FROM windowed w
	WHERE
		w.customer_order_rank <= (SELECT top_orders_per_customer FROM params)
		OR w.customer_running_order_amount > w.history_amount
		OR w.bucket_max_activity_score = w.activity_score
),
named_rows AS (
	SELECT
		fw.warehouse_id,
		fw.district_id,
		fw.customer_id,
		fw.order_id,
		fw.credit_code,
		fw.customer_bucket,
		fw.customer_last,
		fw.district_name,
		fw.warehouse_state,
		fw.order_amount,
		fw.ordered_quantity,
		fw.history_amount,
		fw.history_rows,
		fw.activity_score,
		fw.customer_order_rank,
		fw.customer_running_order_amount,
		fw.prev_order_amount,
		fw.score_gap,
		fw.bucket_max_activity_score,
		CASE
			WHEN fw.score_gap > 0 THEN 'up'
			WHEN fw.score_gap < 0 THEN 'down'
			ELSE 'flat'
		END AS movement_bucket
	FROM filtered_windowed fw
),
final_rows AS (
	SELECT
		nr.warehouse_id,
		nr.district_id,
		nr.customer_id,
		nr.order_id,
		nr.credit_code,
		nr.customer_bucket,
		nr.customer_last,
		nr.district_name,
		nr.warehouse_state,
		nr.order_amount,
		nr.ordered_quantity,
		nr.history_amount,
		nr.history_rows,
		nr.activity_score,
		nr.customer_order_rank,
		nr.customer_running_order_amount,
		nr.prev_order_amount,
		nr.score_gap,
		nr.bucket_max_activity_score,
		nr.movement_bucket
	FROM named_rows nr
	WHERE nr.customer_order_rank > 0
)
SELECT
	warehouse_id,
	district_id,
	customer_id,
	order_id,
	credit_code,
	customer_bucket,
	customer_last,
	district_name,
	warehouse_state,
	order_amount,
	ordered_quantity,
	history_amount,
	history_rows,
	activity_score,
	customer_order_rank,
	customer_running_order_amount,
	prev_order_amount,
	score_gap,
	bucket_max_activity_score,
	movement_bucket
FROM final_rows
ORDER BY
	warehouse_id,
	district_id,
	customer_id,
	customer_order_rank,
	order_id
