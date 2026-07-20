FROM ghcr.io/voidzero-dev/vite-plus:latest AS build
WORKDIR /src
COPY --chown=vp:vp package.json pnpm-lock.yaml pnpm-workspace.yaml .node-version ./
RUN vp install --frozen-lockfile
COPY --chown=vp:vp . ./
RUN vp build

FROM nginx:1.27-alpine
# The official nginx entrypoint renders this template with API_UPSTREAM.
ENV API_UPSTREAM=http://nrl-ota:8080
COPY nginx.conf /etc/nginx/templates/default.conf.template
COPY --from=build /src/dist /usr/share/nginx/html
EXPOSE 80
