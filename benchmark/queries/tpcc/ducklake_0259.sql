-- {"operators": "DUCKLAKE,CTE,INNER_JOIN,LEFT_JOIN,AVG,STDDEV,VAR_POP,AGGREGATE,HAVING,ORDER", "complexity": "high", "is_incremental": true, "has_nulls": true, "has_cast": true, "has_case": true, "tables": "WAREHOUSE,DISTRICT,CUSTOMER,OORDER,ORDER_LINE,HISTORY", "ducklake": true}
WITH
params AS (
	SELECT
		0.0::DOUBLE AS zero_amount,
		'GC' AS good_credit,
		'BC' AS bad_credit,
		1::INTEGER AS min_rows
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
		CAST(d.D_TAX AS DOUBLE) AS district_tax
	FROM dl.DISTRICT d
	WHERE d.D_ID BETWEEN 1 AND 10
),
customer_base AS (
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
		oh.order_id,
		MAX(oh.carrier_id) AS carrier_id,
		SUM(lb.line_amount) AS order_amount,
		SUM(lb.quantity) AS ordered_quantity,
		COUNT(*) AS line_rows
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
customer_activity AS (
	SELECT
		cb.warehouse_id,
		cb.district_id,
		cb.customer_id,
		cb.credit_code,
		cb.balance_amount,
		cb.ytd_payment,
		cb.payment_count,
		cb.delivery_count,
		dd.district_name,
		wd.warehouse_state,
		wd.warehouse_tax,
		COALESCE(orr.order_amount, 0.0) AS order_amount,
		COALESCE(orr.ordered_quantity, 0) AS ordered_quantity,
		COALESCE(orr.line_rows, 0) AS line_rows,
		COALESCE(orr.carrier_id, -1) AS carrier_id,
		COALESCE(hr.history_amount, 0.0) AS history_amount,
		COALESCE(hr.history_rows, 0) AS history_rows
	FROM customer_base cb
	JOIN district_dim dd
		ON dd.warehouse_id = cb.warehouse_id
	   AND dd.district_id = cb.district_id
	JOIN warehouse_dim wd
		ON wd.warehouse_id = cb.warehouse_id
	LEFT JOIN order_rollup orr
		ON orr.warehouse_id = cb.warehouse_id
	   AND orr.district_id = cb.district_id
	   AND orr.customer_id = cb.customer_id
	LEFT JOIN history_rollup hr
		ON hr.warehouse_id = cb.warehouse_id
	   AND hr.district_id = cb.district_id
	   AND hr.customer_id = cb.customer_id
),
metric_inputs AS (
	SELECT
		ca.warehouse_id,
		ca.district_id,
		ca.warehouse_state,
		ca.district_name,
		ca.credit_code,
		CASE
			WHEN ca.credit_code = (SELECT good_credit FROM params) THEN 'good'
			WHEN ca.balance_amount < 0 THEN 'negative'
			ELSE 'review'
		END AS credit_bucket,
		CASE
			WHEN ca.carrier_id = -1 THEN 'open'
			WHEN ca.order_amount > ca.history_amount THEN 'order_heavy'
			ELSE 'payment_heavy'
		END AS activity_bucket,
		CAST(ca.balance_amount AS DOUBLE) AS balance_metric,
		CAST(ca.ytd_payment AS DOUBLE) AS payment_metric,
		CAST(ca.order_amount AS DOUBLE) AS order_metric,
		CAST(ca.history_amount AS DOUBLE) AS history_metric,
		CAST(ca.ordered_quantity AS DOUBLE) AS quantity_metric,
		CASE
			WHEN ca.payment_count = 0 THEN NULL
			ELSE CAST(ca.ytd_payment AS DOUBLE) / NULLIF(CAST(ca.payment_count AS DOUBLE), 0.0)
		END AS avg_payment_metric
	FROM customer_activity ca
),
derived_rollup AS (
	SELECT
		mi.warehouse_id,
		mi.district_id,
		mi.warehouse_state,
		mi.district_name,
		mi.credit_bucket,
		mi.activity_bucket,
		COUNT(*) AS row_count,
		AVG(mi.balance_metric) AS avg_balance,
		AVG(CASE WHEN mi.order_metric > 0 THEN mi.order_metric ELSE NULL END) AS avg_positive_order,
		STDDEV(mi.payment_metric) AS stddev_payment,
		STDDEV_POP(mi.history_metric) AS stddev_pop_history,
		VAR_POP(mi.quantity_metric) AS var_pop_quantity,
		SUM(mi.order_metric) AS order_metric_sum,
		SUM(mi.history_metric) AS history_metric_sum,
		MIN(mi.avg_payment_metric) AS min_avg_payment_metric,
		MAX(mi.avg_payment_metric) AS max_avg_payment_metric
	FROM metric_inputs mi
	GROUP BY
		mi.warehouse_id,
		mi.district_id,
		mi.warehouse_state,
		mi.district_name,
		mi.credit_bucket,
		mi.activity_bucket
	HAVING
		COUNT(*) >= (SELECT min_rows FROM params)
		AND (
			AVG(mi.balance_metric) IS NOT NULL
			OR STDDEV(mi.payment_metric) IS NOT NULL
		)
),
final_rows AS (
	SELECT
		dr.warehouse_id,
		dr.district_id,
		dr.warehouse_state,
		dr.district_name,
		dr.credit_bucket,
		dr.activity_bucket,
		dr.row_count,
		dr.avg_balance,
		COALESCE(dr.avg_positive_order, (SELECT zero_amount FROM params)) AS avg_positive_order,
		COALESCE(dr.stddev_payment, (SELECT zero_amount FROM params)) AS stddev_payment,
		COALESCE(dr.stddev_pop_history, (SELECT zero_amount FROM params)) AS stddev_pop_history,
		COALESCE(dr.var_pop_quantity, (SELECT zero_amount FROM params)) AS var_pop_quantity,
		dr.order_metric_sum,
		dr.history_metric_sum,
		dr.min_avg_payment_metric,
		dr.max_avg_payment_metric
	FROM derived_rollup dr
)
SELECT
	warehouse_id,
	district_id,
	warehouse_state,
	district_name,
	credit_bucket,
	activity_bucket,
	row_count,
	avg_balance,
	avg_positive_order,
	stddev_payment,
	stddev_pop_history,
	var_pop_quantity,
	order_metric_sum,
	history_metric_sum,
	min_avg_payment_metric,
	max_avg_payment_metric
FROM final_rows
ORDER BY
	warehouse_id,
	district_id,
	credit_bucket,
	activity_bucket
