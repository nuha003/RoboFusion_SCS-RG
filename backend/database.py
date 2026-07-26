"""
Database connection setup.
Using SQLite for simplicity.
"""
import os
from sqlalchemy import create_engine, event
from sqlalchemy.orm import sessionmaker
from models import Base

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.path.join(BASE_DIR, "scsrg.db")
SQLALCHEMY_DATABASE_URL = f"sqlite:///{DB_PATH}"

engine = create_engine(
    SQLALCHEMY_DATABASE_URL, connect_args={"check_same_thread": False}
)


# SQLite does NOT enforce foreign keys by default -- without this, zone
# deletion won't actually be blocked even though the model declares the FK
# (Test Case 18b: deleting a zone with open incidents should be blocked).
@event.listens_for(engine, "connect")
def set_sqlite_pragma(dbapi_connection, connection_record):
    cursor = dbapi_connection.cursor()
    cursor.execute("PRAGMA foreign_keys=ON")
    cursor.close()


SessionLocal = sessionmaker(autocommit=False, autoflush=False, bind=engine)


def init_db():
    Base.metadata.create_all(bind=engine)


def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()


if __name__ == "__main__":
    print(f"SQLite DB path: {DB_PATH}")
    print(f"File exists already: {os.path.exists(DB_PATH)}")