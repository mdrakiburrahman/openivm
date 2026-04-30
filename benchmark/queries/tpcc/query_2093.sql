-- {"operators": "FILTER,ANTI_JOIN,NOT_IN,CAST", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": true, "has_case": false, "tables": "STOCK,ORDER_LINE", "non_incr_reason": "join:ANTI"}
SELECT CAST(S_I_ID AS BIGINT) AS item_id FROM STOCK WHERE CAST(S_I_ID AS BIGINT) NOT IN (SELECT CAST(OL_I_ID AS BIGINT) FROM ORDER_LINE);
