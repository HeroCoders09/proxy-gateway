FROM python:3.12-slim
WORKDIR /app
COPY faultproxy.py .
ENV PORT=3128
EXPOSE 3128
CMD ["python", "-u", "faultproxy.py"]
