# config.py
import os
from dotenv import load_dotenv

# Carrega as variáveis de ambiente do arquivo .env
load_dotenv()

class Config:
    # Obtém o host e a porta do servidor Flask das variáveis de ambiente
    # Ou usa valores padrão se não estiverem definidos
    # Define 192.168.18.184 como padrão para o host do servidor na rede local
    SERVER_HOST = os.getenv('FLASK_SERVER_HOST', '192.168.18.184')
    SERVER_PORT = int(os.getenv('FLASK_SERVER_PORT', 5000))

    # Obtém a URL do banco de dados PostgreSQL
    DATABASE_URL = os.getenv("DATABASE_URL")
    
    # Nome do banco de dados (útil para mensagens de log)
    DB_NAME = os.getenv("DB_NAME", "DADOS_CONTAGEM") # Nome do seu banco de dados
    
    # Permite aceitar contagem mesmo fora do horário de turno (sem gravar em DB)
    IGNORE_SHIFT_CHECK = os.getenv('IGNORE_SHIFT_CHECK', 'false').lower() in ['1', 'true', 'yes', 'on']

    # HTTPS / SSL
    SSL_ENABLED = os.getenv('SSL_ENABLED', 'false').lower() in ['1', 'true', 'yes', 'on']
    SSL_CERT_FILE = os.getenv('SSL_CERT_FILE', os.path.join(os.path.dirname(__file__), 'cert.pem'))
    SSL_KEY_FILE = os.getenv('SSL_KEY_FILE', os.path.join(os.path.dirname(__file__), 'key.pem'))