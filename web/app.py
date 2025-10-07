import os
from dotenv import load_dotenv
from flask import Flask, jsonify
import psycopg2
from psycopg2 import sql

# Carrega as variáveis de ambiente do arquivo .env
load_dotenv()

app = Flask(__name__)

def get_db_connection():
    try:
        conn = psycopg2.connect(os.getenv("DATABASE_URL"))
        return conn
    except psycopg2.OperationalError as e:
        print(f"Erro ao conectar ao banco de dados: {e}")
        return None

# Variável global para armazenar a contagem em memória,
# mas o valor inicial será carregado do banco de dados
contador_em_memoria = 0