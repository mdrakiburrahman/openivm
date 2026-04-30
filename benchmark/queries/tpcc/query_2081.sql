-- {"operators": "FILTER,ANTI_JOIN,SUBQUERY,AGGREGATE", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "WAREHOUSE,STOCK", "non_incr_reason": "join:ANTI"}
SELECT w.W_ID, w.W_NAME FROM WAREHOUSE w WHERE NOT EXISTS (SELECT 1 FROM STOCK s WHERE s.S_W_ID = w.W_ID GROUP BY s.S_W_ID HAVING SUM(s.S_QUANTITY) > 1000);
