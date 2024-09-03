-- SQLite
-- SELECT victim,bit,flipped_to,count,GROUP_CONCAT(aggressor || " " || aggressor_init) FROM (SELECT *,COUNT(victim) AS count FROM bitflips GROUP BY victim HAVING COUNT(victim) = 3) AS fl JOIN aggressors ON fl.test_id = aggressors.test_id JOIN tests on fl.test_id = tests.id GROUP BY victim;

-- filter the victims on the number of templating tests that discoved them, and group the relevant info
SELECT victim, bit, flipped_to, GROUP_CONCAT(aggressor) AS aggressors, aggressor_init, count AS count_olaf
FROM (SELECT *, COUNT(victim) AS count
      FROM bitflips
      GROUP BY victim
      HAVING COUNT(victim) > ###your_threshold###) AS fl
JOIN aggressors ON fl.test_id = aggressors.test_id
JOIN tests ON fl.test_id = tests.id
GROUP BY victim;

-- TODO multiple bitflips in the same victim are grouped away

