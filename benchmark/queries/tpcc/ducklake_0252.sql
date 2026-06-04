-- {"operators": "DUCKLAKE,CTE,REPEATED_CTE,SELF_JOIN,INNER_JOIN,LEFT_JOIN,AGGREGATE,HAVING,ORDER", "complexity": "high", "is_incremental": true, "has_nulls": true, "has_cast": true, "has_case": true, "tables": "CUSTOMER,DISTRICT,WAREHOUSE,HISTORY,OORDER", "ducklake": true}
WITH
params AS (
	SELECT
		'GC' AS good_credit,
		'BC' AS bad_credit,
		5::INTEGER AS neighbor_gap,
		0.0::DOUBLE AS zero_amount,
		2::INTEGER AS min_peer_rows
),
warehouse_dim AS (
	SELECT
		w.W_ID AS warehouse_id,
		LOWER(TRIM(w.W_NAME)) AS warehouse_name,
		COALESCE(w.W_STATE, 'NA') AS warehouse_state,
		CAST(w.W_YTD AS DOUBLE) AS warehouse_ytd
	FROM dl.WAREHOUSE w
	WHERE w.W_ID IS NOT NULL
),
district_dim AS (
	SELECT
		d.D_W_ID AS warehouse_id,
		d.D_ID AS district_id,
		LOWER(TRIM(d.D_NAME)) AS district_name,
		CAST(d.D_YTD AS DOUBLE) AS district_ytd,
		d.D_NEXT_O_ID AS next_order_id
	FROM dl.DISTRICT d
	WHERE d.D_ID BETWEEN 1 AND 10
),
customer_source AS (
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
customer_dim AS (
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
		dd.district_name,
		dd.next_order_id,
		wd.warehouse_state
	FROM customer_source cs
	JOIN district_dim dd
		ON dd.warehouse_id = cs.warehouse_id
	   AND dd.district_id = cs.district_id
	JOIN warehouse_dim wd
		ON wd.warehouse_id = cs.warehouse_id
),
customer_bucketed AS (
	SELECT
		cd.*,
		CASE
			WHEN cd.credit_code = (SELECT good_credit FROM params) THEN 'good'
			WHEN cd.balance_amount < 0 THEN 'negative'
			ELSE 'review'
		END AS customer_bucket,
		CASE
			WHEN cd.payment_count = 0 THEN 0.0
			ELSE cd.ytd_payment / NULLIF(cd.payment_count, 0)
		END AS avg_payment
	FROM customer_dim cd
),
customer_reused AS (
	SELECT
		cb.warehouse_id,
		cb.district_id,
		cb.customer_id,
		cb.customer_last,
		cb.customer_first,
		cb.credit_code,
		cb.customer_bucket,
		cb.balance_amount,
		cb.ytd_payment,
		cb.avg_payment,
		cb.delivery_count,
		cb.district_name,
		cb.warehouse_state
	FROM customer_bucketed cb
	WHERE cb.customer_id BETWEEN 1 AND 3000
),
history_source AS (
	SELECT
		h.H_C_W_ID AS warehouse_id,
		h.H_C_D_ID AS district_id,
		h.H_C_ID AS customer_id,
		CAST(h.H_AMOUNT AS DOUBLE) AS history_amount
	FROM dl.HISTORY h
	WHERE h.H_C_ID IS NOT NULL
),
history_rollup AS (
	SELECT
		hs.warehouse_id,
		hs.district_id,
		hs.customer_id,
		SUM(hs.history_amount) AS history_amount,
		COUNT(*) AS history_rows
	FROM history_source hs
	GROUP BY
		hs.warehouse_id,
		hs.district_id,
		hs.customer_id
),
order_source AS (
	SELECT
		o.O_W_ID AS warehouse_id,
		o.O_D_ID AS district_id,
		o.O_C_ID AS customer_id,
		o.O_ID AS order_id,
		CAST(o.O_OL_CNT AS BIGINT) AS order_line_count
	FROM dl.OORDER o
	WHERE o.O_ID IS NOT NULL
),
order_rollup AS (
	SELECT
		os.warehouse_id,
		os.district_id,
		os.customer_id,
		COUNT(*) AS order_rows,
		SUM(os.order_line_count) AS total_order_lines
	FROM order_source os
	GROUP BY
		os.warehouse_id,
		os.district_id,
		os.customer_id
),
peer_pairs AS (
	SELECT
		l.warehouse_id,
		l.district_id,
		l.customer_id AS left_customer_id,
		r.customer_id AS right_customer_id,
		l.customer_bucket AS left_bucket,
		r.customer_bucket AS right_bucket,
		l.credit_code AS left_credit,
		r.credit_code AS right_credit,
		l.balance_amount AS left_balance,
		r.balance_amount AS right_balance,
		l.avg_payment AS left_avg_payment,
		r.avg_payment AS right_avg_payment,
		l.warehouse_state,
		l.district_name
	FROM customer_reused l
	JOIN customer_reused r
		ON r.warehouse_id = l.warehouse_id
	   AND r.district_id = l.district_id
	   AND r.customer_id > l.customer_id
	   AND r.customer_id <= l.customer_id + (SELECT neighbor_gap FROM params)
),
peer_with_left_activity AS (
	SELECT
		pp.*,
		COALESCE(hl.history_amount, 0.0) AS left_history_amount,
		COALESCE(hl.history_rows, 0) AS left_history_rows,
		COALESCE(ol.order_rows, 0) AS left_order_rows,
		COALESCE(ol.total_order_lines, 0) AS left_order_lines
	FROM peer_pairs pp
	LEFT JOIN history_rollup hl
		ON hl.warehouse_id = pp.warehouse_id
	   AND hl.district_id = pp.district_id
	   AND hl.customer_id = pp.left_customer_id
	LEFT JOIN order_rollup ol
		ON ol.warehouse_id = pp.warehouse_id
	   AND ol.district_id = pp.district_id
	   AND ol.customer_id = pp.left_customer_id
),
peer_with_both_activity AS (
	SELECT
		pl.*,
		COALESCE(hr.history_amount, 0.0) AS right_history_amount,
		COALESCE(hr.history_rows, 0) AS right_history_rows,
		COALESCE(orr.order_rows, 0) AS right_order_rows,
		COALESCE(orr.total_order_lines, 0) AS right_order_lines
	FROM peer_with_left_activity pl
	LEFT JOIN history_rollup hr
		ON hr.warehouse_id = pl.warehouse_id
	   AND hr.district_id = pl.district_id
	   AND hr.customer_id = pl.right_customer_id
	LEFT JOIN order_rollup orr
		ON orr.warehouse_id = pl.warehouse_id
	   AND orr.district_id = pl.district_id
	   AND orr.customer_id = pl.right_customer_id
),
peer_scored AS (
	SELECT
		pba.*,
		ABS(pba.left_balance - pba.right_balance) AS balance_gap,
		ABS(pba.left_avg_payment - pba.right_avg_payment) AS payment_gap,
		pba.left_order_rows + pba.right_order_rows AS pair_order_rows,
		pba.left_history_rows + pba.right_history_rows AS pair_history_rows
	FROM peer_with_both_activity pba
	WHERE pba.left_bucket <> 'review' OR pba.right_bucket <> 'review'
),
rollup AS (
	SELECT
		ps.warehouse_id,
		ps.district_id,
		ps.warehouse_state,
		ps.district_name,
		ps.left_bucket,
		ps.right_bucket,
		COUNT(*) AS peer_rows,
		SUM(ps.balance_gap) AS balance_gap_sum,
		SUM(ps.payment_gap) AS payment_gap_sum,
		SUM(ps.pair_order_rows) AS pair_order_rows_sum,
		SUM(ps.pair_history_rows) AS pair_history_rows_sum,
		MIN(ps.balance_gap) AS min_balance_gap,
		MAX(ps.balance_gap) AS max_balance_gap
	FROM peer_scored ps
	GROUP BY
		ps.warehouse_id,
		ps.district_id,
		ps.warehouse_state,
		ps.district_name,
		ps.left_bucket,
		ps.right_bucket
	HAVING COUNT(*) >= (SELECT min_peer_rows FROM params)
),
final_rows AS (
	SELECT
		r.warehouse_id,
		r.district_id,
		r.warehouse_state,
		r.district_name,
		r.left_bucket,
		r.right_bucket,
		r.peer_rows,
		r.balance_gap_sum,
		r.payment_gap_sum,
		r.pair_order_rows_sum,
		r.pair_history_rows_sum,
		r.min_balance_gap,
		r.max_balance_gap,
		CASE
			WHEN r.peer_rows = 0 THEN (SELECT zero_amount FROM params)
			ELSE r.balance_gap_sum / NULLIF(r.peer_rows, 0)
		END AS avg_balance_gap
	FROM rollup r
)
SELECT
	warehouse_id,
	district_id,
	warehouse_state,
	district_name,
	left_bucket,
	right_bucket,
	peer_rows,
	balance_gap_sum,
	payment_gap_sum,
	pair_order_rows_sum,
	pair_history_rows_sum,
	min_balance_gap,
	max_balance_gap,
	avg_balance_gap
FROM final_rows
ORDER BY
	warehouse_id,
	district_id,
	left_bucket,
	right_bucket
