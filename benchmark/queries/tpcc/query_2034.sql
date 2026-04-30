-- {"operators": "UNNEST,TABLE_FUNCTION,INNER_JOIN", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,STOCK", "non_incr_reason": "op:UNNEST"}
SELECT i.I_ID, i.I_NAME, tag.tag FROM ITEM i JOIN STOCK s ON i.I_ID = s.S_I_ID CROSS JOIN UNNEST(['stocked', 'priced']) AS tag(tag) WHERE s.S_QUANTITY > 50;
