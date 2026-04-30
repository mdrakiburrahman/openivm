-- {"operators": "EXCEPT_ALL,CAST", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": true, "has_case": false, "tables": "STOCK,ORDER_LINE", "non_incr_reason": "kw:EXCEPT_ALL"}
SELECT CAST(S_W_ID AS BIGINT), CAST(S_I_ID AS BIGINT) FROM STOCK EXCEPT ALL SELECT CAST(OL_SUPPLY_W_ID AS BIGINT), CAST(OL_I_ID AS BIGINT) FROM ORDER_LINE;
