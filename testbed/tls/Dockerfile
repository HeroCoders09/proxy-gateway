FROM debian:bookworm-slim
RUN apt-get update \
    && apt-get install -y --no-install-recommends stunnel4 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY stunnel.conf /etc/stunnel/stunnel.conf
EXPOSE 8443
CMD ["stunnel4", "/etc/stunnel/stunnel.conf"]
