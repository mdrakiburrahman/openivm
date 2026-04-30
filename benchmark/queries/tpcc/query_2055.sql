-- {"operators": "EXCEPT_ALL,AGGREGATE", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "CUSTOMER,OORDER", "non_incr_reason": "kw:EXCEPT_ALL"}
SELECT C_W_ID, C_D_ID, COUNT(*) AS cnt FROM CUSTOMER GROUP BY C_W_ID, C_D_ID EXCEPT ALL SELECT O_W_ID, O_D_ID, COUNT(*) FROM OORDER GROUP BY O_W_ID, O_D_ID;
