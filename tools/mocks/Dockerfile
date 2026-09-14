# Twilio Messages API mock -- pager v2 Phase 5 (docs/SERVER_PLAN.md §6.4/§8
# scenario 7). Standalone image (not the relay's own Dockerfile) since this
# is a *test double*, never shipped to a real deployment.

FROM python:3.12-slim

WORKDIR /app

COPY twilio_mock.py .

# python-multipart: FastAPI's `Request.form()` needs it to parse the
# application/x-www-form-urlencoded body Twilio's real Messages API (and
# this mock's `/2010-04-01/Accounts/{Sid}/Messages.json` endpoint) expects.
RUN pip install --no-cache-dir fastapi "uvicorn[standard]" python-multipart

EXPOSE 8010

CMD ["python3", "twilio_mock.py", "--port", "8010"]
