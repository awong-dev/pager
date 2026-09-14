# Firestore + Auth emulator image for local dev / CI (docs/SERVER_PLAN.md
# §5.9, §9.1's `relay/emulator.Dockerfile`). The Firebase Emulator Suite is a
# Node CLI (`firebase-tools`) that shells out to a bundled Firestore
# emulator JAR, so it needs a JRE -- that's the whole reason this is a
# separate image from the relay's own `python:3.12-slim` one.
#
# firebase-tools (>=14) requires a JRE >= 21; Debian bookworm's own
# `default-jre-headless`/`openjdk-21-jre-headless` packages only go up to 17,
# so this pulls Eclipse Temurin 21 from Adoptium's apt repo instead of the
# `default-jre-headless` most other Dockerfiles in this repo use.
FROM node:20-bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends wget gnupg ca-certificates \
    && wget -qO - https://packages.adoptium.net/artifactory/api/gpg/key/public \
        | gpg --dearmor -o /etc/apt/trusted.gpg.d/adoptium.gpg \
    && echo "deb https://packages.adoptium.net/artifactory/deb $(awk -F= '/^VERSION_CODENAME/{print $2}' /etc/os-release) main" \
        > /etc/apt/sources.list.d/adoptium.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends temurin-21-jre \
    && apt-get purge -y wget gnupg \
    && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/*

RUN npm install -g firebase-tools

WORKDIR /srv

# Rules/indexes + firebase.json/.firebaserc (project config: a `demo-`
# project id needs no real credentials or `firebase login`, per the
# Emulator Suite's own convention).
COPY firebase.json .firebaserc firestore.rules firestore.indexes.json ./

EXPOSE 8080 9099 4000

CMD ["firebase", "emulators:start", "--only", "firestore,auth", "--project", "demo-pager"]
