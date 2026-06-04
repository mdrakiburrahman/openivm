-- {"operators": "FILTER,SEMI_JOIN,SUBQUERY,AGGREGATE,HAVING", "complexity": "high", "is_incremental": true, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,ORDER_LINE"}
SELECT i.I_IM_ID, COUNT(*) AS item_count FROM ITEM i WHERE EXISTS (SELECT 1 FROM ORDER_LINE ol WHERE ol.OL_I_ID = i.I_ID GROUP BY ol.OL_I_ID HAVING SUM(ol.OL_AMOUNT) > 0) GROUP BY i.I_IM_ID;
