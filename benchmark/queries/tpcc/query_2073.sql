-- {"operators": "FILTER,SEMI_JOIN,SUBQUERY,WINDOW", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,STOCK", "non_incr_reason": "join:SEMI"}
SELECT i.I_ID, ROW_NUMBER() OVER (ORDER BY i.I_PRICE DESC) AS price_rank FROM ITEM i WHERE EXISTS (SELECT 1 FROM STOCK s WHERE s.S_I_ID = i.I_ID);
