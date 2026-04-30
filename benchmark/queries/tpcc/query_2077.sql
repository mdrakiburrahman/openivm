-- {"operators": "FILTER,SEMI_JOIN,IN,DISTINCT", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "STOCK,ORDER_LINE", "non_incr_reason": "join:SEMI"}
SELECT s.S_W_ID, s.S_I_ID FROM STOCK s WHERE s.S_I_ID IN (SELECT DISTINCT OL_I_ID FROM ORDER_LINE);
