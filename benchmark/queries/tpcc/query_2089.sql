-- {"operators": "FILTER,ANTI_JOIN,NOT_IN,DISTINCT", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "STOCK,ORDER_LINE", "non_incr_reason": "join:ANTI"}
SELECT s.S_W_ID, s.S_I_ID FROM STOCK s WHERE s.S_I_ID NOT IN (SELECT DISTINCT OL_I_ID FROM ORDER_LINE);
