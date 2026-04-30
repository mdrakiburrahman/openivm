-- {"operators": "FILTER,ANTI_JOIN,SUBQUERY,EXCEPT", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,STOCK,ORDER_LINE", "non_incr_reason": "join:ANTI"}
SELECT i.I_ID FROM ITEM i WHERE i.I_ID NOT IN (SELECT S_I_ID FROM STOCK EXCEPT SELECT OL_I_ID FROM ORDER_LINE);
