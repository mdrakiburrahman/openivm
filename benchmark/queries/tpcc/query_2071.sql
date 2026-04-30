-- {"operators": "FILTER,SEMI_JOIN,ALL", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,ORDER_LINE", "non_incr_reason": "join:SEMI"}
SELECT i.I_ID, i.I_NAME FROM ITEM i WHERE i.I_PRICE >= ALL (SELECT ol.OL_AMOUNT FROM ORDER_LINE ol WHERE ol.OL_I_ID = i.I_ID);
