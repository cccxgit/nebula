import json
import os
import time
from collections import defaultdict
from pathlib import Path

from nebula3.Config import Config
from nebula3.gclient.net import ConnectionPool


DATA_DIR = Path(os.getenv("YELP_DATA_DIR", "/home/sch/tasks/yelp_data/extracted"))
TARGET_CITIES = {
    city.strip() for city in os.getenv("YELP_TARGET_CITIES", "Phoenix,Las Vegas").split(",") if city.strip()
}
TARGET_CATEGORIES = {
    "Restaurants",
    "Coffee & Tea",
    "Desserts",
    "Bubble Tea",
    "Hot Pot",
    "Bars",
    "Breakfast & Brunch",
    "Pizza",
    "Steakhouses",
}
SPACE = "yelp_graph"
MAX_BUSINESSES = int(os.getenv("YELP_MAX_BUSINESSES", "400"))
MAX_REVIEWS_PER_BUSINESS = int(os.getenv("YELP_MAX_REVIEWS_PER_BUSINESS", "15"))
MAX_TOTAL_REVIEWS = int(os.getenv("YELP_MAX_TOTAL_REVIEWS", "5000"))


def escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ").replace("\r", " ")


def normalize_id(prefix: str, raw_id: str) -> str:
    return f"{prefix}_{raw_id}"


def normalize_category(name: str) -> str:
    return "cat_" + "_".join(name.strip().lower().replace("&", "and").replace("/", " ").split())


def normalize_city(city: str) -> str:
    return "city_" + "_".join(city.strip().lower().split())


def get_files():
    business_file = next(DATA_DIR.rglob("*business*.json"))
    review_file = next(DATA_DIR.rglob("*review*.json"))
    user_file = next(DATA_DIR.rglob("*user*.json"))
    return business_file, review_file, user_file


def load_business_subset(business_file: Path):
    businesses = {}
    categories = set()
    business_categories = defaultdict(list)
    city_vertices = {}

    with business_file.open("r", encoding="utf-8") as f:
        for line in f:
            record = json.loads(line)
            city = (record.get("city") or "").strip()
            cats = record.get("categories") or ""
            if isinstance(cats, list):
                cat_list = [str(c).strip() for c in cats if str(c).strip()]
            else:
                cat_list = [c.strip() for c in str(cats).split(",") if c.strip()]
            if city not in TARGET_CITIES:
                continue
            if not any(c in TARGET_CATEGORIES for c in cat_list):
                continue
            business_id = record["business_id"]
            businesses[business_id] = {
                "vid": normalize_id("biz", business_id),
                "raw_business_id": business_id,
                "name": record.get("name", ""),
                "stars": float(record.get("stars") or 0.0),
                "review_count": int(record.get("review_count") or 0),
                "city": city,
                "state": record.get("state", ""),
                "postal_code": record.get("postal_code", ""),
                "latitude": float(record.get("latitude") or 0.0),
                "longitude": float(record.get("longitude") or 0.0),
                "is_open": bool(record.get("is_open")),
            }
            for cat in cat_list:
                categories.add(cat)
                business_categories[business_id].append(cat)
            city_vertices[city] = {
                "vid": normalize_city(city),
                "city_name": city,
                "state": record.get("state", ""),
            }
            if len(businesses) >= MAX_BUSINESSES:
                break

    return businesses, categories, business_categories, city_vertices


def load_reviews_subset(review_file: Path, allowed_businesses: set[str]):
    reviews = {}
    business_review_counts = defaultdict(int)
    user_ids = set()

    with review_file.open("r", encoding="utf-8") as f:
        for line in f:
            if len(reviews) >= MAX_TOTAL_REVIEWS:
                break
            record = json.loads(line)
            business_id = record.get("business_id")
            if business_id not in allowed_businesses:
                continue
            if business_review_counts[business_id] >= MAX_REVIEWS_PER_BUSINESS:
                continue

            review_id = record["review_id"]
            user_id = record["user_id"]
            reviews[review_id] = {
                "vid": normalize_id("review", review_id),
                "raw_review_id": review_id,
                "business_id": business_id,
                "user_id": user_id,
                "stars": float(record.get("stars") or 0.0),
                "review_date": record.get("date", ""),
                "text": (record.get("text") or "")[:2000],
            }
            business_review_counts[business_id] += 1
            user_ids.add(user_id)

    return reviews, user_ids


def load_users_subset(user_file: Path, allowed_users: set[str]):
    users = {}
    with user_file.open("r", encoding="utf-8") as f:
        for line in f:
            record = json.loads(line)
            user_id = record.get("user_id")
            if user_id not in allowed_users:
                continue
            users[user_id] = {
                "vid": normalize_id("user", user_id),
                "raw_user_id": user_id,
                "review_count": int(record.get("review_count") or 0),
                "average_stars": float(record.get("average_stars") or 0.0),
                "useful": int(record.get("useful") or 0),
                "funny": int(record.get("funny") or 0),
                "cool": int(record.get("cool") or 0),
                "fans": int(record.get("fans") or 0),
            }
            if len(users) == len(allowed_users):
                break
    return users


def batches(items, size):
    batch = []
    for item in items:
        batch.append(item)
        if len(batch) >= size:
            yield batch
            batch = []
    if batch:
        yield batch


def exec_or_raise(session, command: str):
    result = session.execute(command)
    if not result.is_succeeded():
        raise RuntimeError(f"nGQL failed: {command}\n{result.error_msg()}")


def create_schema(session):
    exec_or_raise(session, f"CREATE SPACE IF NOT EXISTS {SPACE}(vid_type=FIXED_STRING(128), partition_num=20, replica_factor=1);")
    time.sleep(30)
    exec_or_raise(session, f"USE {SPACE}")
    exec_or_raise(session, """
CREATE TAG IF NOT EXISTS user(
  raw_user_id STRING NOT NULL,
  review_count INT64,
  average_stars DOUBLE,
  useful INT64,
  funny INT64,
  cool INT64,
  fans INT64
);""")
    exec_or_raise(session, """
CREATE TAG IF NOT EXISTS business(
  raw_business_id STRING NOT NULL,
  name STRING NOT NULL,
  stars DOUBLE,
  review_count INT64,
  city STRING,
  state STRING,
  postal_code STRING,
  latitude DOUBLE,
  longitude DOUBLE,
  is_open BOOL
);""")
    exec_or_raise(session, """
CREATE TAG IF NOT EXISTS review(
  raw_review_id STRING NOT NULL,
  stars DOUBLE,
  review_date STRING,
  text STRING
);""")
    exec_or_raise(session, """
CREATE TAG IF NOT EXISTS category(
  category_name STRING NOT NULL
);""")
    exec_or_raise(session, """
CREATE TAG IF NOT EXISTS city(
  city_name STRING NOT NULL,
  state STRING
);""")
    exec_or_raise(session, "CREATE EDGE IF NOT EXISTS WROTE(review_date STRING, stars DOUBLE);")
    exec_or_raise(session, "CREATE EDGE IF NOT EXISTS REVIEWS(stars DOUBLE);")
    exec_or_raise(session, "CREATE EDGE IF NOT EXISTS HAS_CATEGORY();")
    exec_or_raise(session, "CREATE EDGE IF NOT EXISTS LOCATED_IN();")
    time.sleep(15)


def insert_vertices(session, users, businesses, reviews, categories, city_vertices):
    exec_or_raise(session, f"USE {SPACE}")

    for batch in batches(users.values(), 100):
        values = ", ".join(
            f'"{u["vid"]}":("{escape(u["raw_user_id"])}",{u["review_count"]},{u["average_stars"]},{u["useful"]},{u["funny"]},{u["cool"]},{u["fans"]})'
            for u in batch
        )
        exec_or_raise(session, f"INSERT VERTEX user(raw_user_id,review_count,average_stars,useful,funny,cool,fans) VALUES {values};")

    for batch in batches(businesses.values(), 80):
        values = ", ".join(
            f'"{b["vid"]}":("{escape(b["raw_business_id"])}","{escape(b["name"])}",{b["stars"]},{b["review_count"]},"{escape(b["city"])}","{escape(b["state"])}","{escape(b["postal_code"])}",{b["latitude"]},{b["longitude"]},{str(b["is_open"]).lower()})'
            for b in batch
        )
        exec_or_raise(session, f"INSERT VERTEX business(raw_business_id,name,stars,review_count,city,state,postal_code,latitude,longitude,is_open) VALUES {values};")

    for batch in batches(reviews.values(), 80):
        values = ", ".join(
            f'"{r["vid"]}":("{escape(r["raw_review_id"])}",{r["stars"]},"{escape(r["review_date"])}","{escape(r["text"])}")'
            for r in batch
        )
        exec_or_raise(session, f'INSERT VERTEX review(raw_review_id,stars,review_date,text) VALUES {values};')

    category_vertices = [{"vid": normalize_category(c), "category_name": c} for c in sorted(categories)]
    for batch in batches(category_vertices, 100):
        values = ", ".join(
            f'"{c["vid"]}":("{escape(c["category_name"])}")' for c in batch
        )
        exec_or_raise(session, f'INSERT VERTEX category(category_name) VALUES {values};')

    for batch in batches(city_vertices.values(), 100):
        values = ", ".join(
            f'"{c["vid"]}":("{escape(c["city_name"])}","{escape(c["state"])}")' for c in batch
        )
        exec_or_raise(session, f'INSERT VERTEX city(city_name,state) VALUES {values};')


def insert_edges(session, businesses, reviews, business_categories, city_vertices, users):
    exec_or_raise(session, f"USE {SPACE}")

    wrote_edges = []
    reviews_edges = []
    for r in reviews.values():
        if r["user_id"] in users:
            wrote_edges.append(
                f'"{normalize_id("user", r["user_id"])}"->"{r["vid"]}":("{escape(r["review_date"])}",{r["stars"]})'
            )
        reviews_edges.append(
            f'"{r["vid"]}"->"{businesses[r["business_id"]]["vid"]}":({r["stars"]})'
        )

    for batch in batches(wrote_edges, 100):
        exec_or_raise(session, f"INSERT EDGE WROTE(review_date,stars) VALUES {', '.join(batch)};")
    for batch in batches(reviews_edges, 100):
        exec_or_raise(session, f"INSERT EDGE REVIEWS(stars) VALUES {', '.join(batch)};")

    category_edges = []
    city_edges = []
    for business_id, business in businesses.items():
        city_edges.append(f'"{business["vid"]}"->"{normalize_city(business["city"])}":()')
        for cat in business_categories[business_id]:
            category_edges.append(f'"{business["vid"]}"->"{normalize_category(cat)}":()')

    for batch in batches(category_edges, 100):
        exec_or_raise(session, f"INSERT EDGE HAS_CATEGORY() VALUES {', '.join(batch)};")
    for batch in batches(city_edges, 100):
        exec_or_raise(session, f"INSERT EDGE LOCATED_IN() VALUES {', '.join(batch)};")


def create_indexes(session):
    exec_or_raise(session, f"USE {SPACE}")
    exec_or_raise(session, "CREATE TAG INDEX IF NOT EXISTS idx_business_name ON business(name(128));")
    exec_or_raise(session, "CREATE TAG INDEX IF NOT EXISTS idx_business_city ON business(city(64));")
    exec_or_raise(session, "CREATE TAG INDEX IF NOT EXISTS idx_category_name ON category(category_name(128));")
    exec_or_raise(session, "CREATE TAG INDEX IF NOT EXISTS idx_city_name ON city(city_name(128));")
    time.sleep(15)
    exec_or_raise(session, "REBUILD TAG INDEX idx_business_name;")
    exec_or_raise(session, "REBUILD TAG INDEX idx_business_city;")
    exec_or_raise(session, "REBUILD TAG INDEX idx_category_name;")
    exec_or_raise(session, "REBUILD TAG INDEX idx_city_name;")


def main():
    host = os.getenv("NEBULA_HOST", "127.0.0.1")
    port = int(os.getenv("NEBULA_PORT", "9669"))
    user = os.getenv("NEBULA_USER", "root")
    password = os.getenv("NEBULA_PASSWORD", "nebula")

    business_file, review_file, user_file = get_files()
    print(f"Using files:\n- {business_file}\n- {review_file}\n- {user_file}")

    businesses, categories, business_categories, city_vertices = load_business_subset(business_file)
    reviews, user_ids = load_reviews_subset(review_file, set(businesses.keys()))
    users = load_users_subset(user_file, user_ids)

    print(
        f"Prepared subset: {len(businesses)} businesses, {len(reviews)} reviews, "
        f"{len(users)} users, {len(categories)} categories, {len(city_vertices)} cities"
    )

    config = Config()
    config.max_connection_pool_size = 10
    pool = ConnectionPool()
    if not pool.init([(host, port)], config):
        raise RuntimeError("Failed to initialize NebulaGraph connection pool")

    session = pool.get_session(user, password)
    try:
        create_schema(session)
        insert_vertices(session, users, businesses, reviews, categories, city_vertices)
        insert_edges(session, businesses, reviews, business_categories, city_vertices, users)
        create_indexes(session)
        print("Import completed successfully.")
    finally:
        session.release()
        pool.close()


if __name__ == "__main__":
    main()
