import os

from nebula3.Config import Config
from nebula3.gclient.net import ConnectionPool


QUERIES = [
    ("SHOW TAGS", "yelp_graph"),
    ("SHOW EDGES", "yelp_graph"),
    ("MATCH (b:business) RETURN COUNT(b) AS business_count", "yelp_graph"),
    ("MATCH (r:review) RETURN COUNT(r) AS review_count", "yelp_graph"),
    (
        'MATCH (b:business)-[:LOCATED_IN]->(c:city) WHERE c.city.city_name == "Cleveland" RETURN b.business.name, b.business.stars LIMIT 5',
        "yelp_graph",
    ),
]


def main():
    host = os.getenv("NEBULA_HOST", "127.0.0.1")
    port = int(os.getenv("NEBULA_PORT", "9669"))
    user = os.getenv("NEBULA_USER", "root")
    password = os.getenv("NEBULA_PASSWORD", "nebula")

    config = Config()
    config.max_connection_pool_size = 5
    pool = ConnectionPool()
    pool.init([(host, port)], config)
    session = pool.get_session(user, password)
    try:
        for query, space in QUERIES:
            if space:
                session.execute(f"USE {space}")
            result = session.execute(query)
            print(f"\nQUERY: {query}")
            if not result.is_succeeded():
                print(f"FAILED: {result.error_msg()}")
                continue
            print("SUCCEEDED")
            if result.row_size() > 0:
                print(" | ".join(result.keys()))
                for i in range(min(result.row_size(), 10)):
                    row = result.row_values(i)
                    print(" | ".join(str(v) for v in row))
    finally:
        session.release()
        pool.close()


if __name__ == "__main__":
    main()
