FROM python:3.12-slim

WORKDIR /app

COPY pyproject.toml .
COPY app/ app/
COPY static/ static/

RUN pip install --no-cache-dir .

# TODO(orchestrator): app.main:app created in Phase 2
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
