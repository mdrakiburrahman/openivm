-- {"operators": "FILTER,ANTI_JOIN,NOT_IN,CTE", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,ORDER_LINE", "non_incr_reason": "join:ANTI"}
WITH hot_items AS (SELECT OL_I_ID FROM ORDER_LINE GROUP BY OL_I_ID HAVING SUM(OL_QUANTITY) > 5) SELECT i.I_ID, i.I_NAME FROM ITEM i WHERE i.I_ID NOT IN (SELECT OL_I_ID FROM hot_items);
