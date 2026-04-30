-- {"operators": "INTERSECT_ALL,CTE", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "OORDER,NEW_ORDER", "non_incr_reason": "kw:INTERSECT_ALL"}
WITH open_orders AS (SELECT O_W_ID, O_D_ID, O_ID FROM OORDER WHERE O_CARRIER_ID IS NULL) SELECT * FROM open_orders INTERSECT ALL SELECT NO_W_ID, NO_D_ID, NO_O_ID FROM NEW_ORDER;
