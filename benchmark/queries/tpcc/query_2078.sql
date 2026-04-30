-- {"operators": "FILTER,SEMI_JOIN,SUBQUERY,UNION", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,STOCK,ORDER_LINE", "non_incr_reason": "join:SEMI"}
SELECT i.I_ID FROM ITEM i WHERE i.I_ID IN (SELECT S_I_ID FROM STOCK UNION SELECT OL_I_ID FROM ORDER_LINE);
