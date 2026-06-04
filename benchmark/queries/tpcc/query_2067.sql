-- {"operators": "FILTER,SEMI_JOIN,SUBQUERY,AGGREGATE", "complexity": "high", "is_incremental": true, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "WAREHOUSE,STOCK"}
SELECT w.W_ID, w.W_NAME FROM WAREHOUSE w WHERE EXISTS (SELECT 1 FROM STOCK s WHERE s.S_W_ID = w.W_ID GROUP BY s.S_W_ID HAVING SUM(s.S_QUANTITY) > 1000);
