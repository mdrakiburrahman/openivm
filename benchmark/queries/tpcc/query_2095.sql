-- {"operators": "ASOF_JOIN,LEFT_JOIN", "complexity": "high", "is_incremental": true, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ORDER_LINE,OORDER"}
SELECT ol.OL_W_ID, ol.OL_I_ID, o.O_C_ID FROM ORDER_LINE ol ASOF LEFT JOIN OORDER o ON ol.OL_W_ID = o.O_W_ID AND ol.OL_D_ID = o.O_D_ID AND ol.OL_DELIVERY_D >= o.O_ENTRY_D;
