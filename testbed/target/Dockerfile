FROM python:3.12-slim
WORKDIR /app
COPY server.py .
ENV PORT=80
EXPOSE 80
HEALTHCHECK --interval=2s --timeout=2s --retries=15 CMD python -c "import urllib.request as u; u.urlopen('http://127.0.0.1:80/health', timeout=1)"
CMD ["python", "-u", "server.py"]
