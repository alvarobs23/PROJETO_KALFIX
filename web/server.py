# server.py
from flask import Flask, render_template, request, jsonify
from flask_socketio import SocketIO
from datetime import datetime, time, timedelta
import sys
import os

# Adiciona o diretório atual ao path para imports
sys.path.append(os.path.dirname(os.path.abspath(__file__)))

# Importa o db_manager que é uma instância da classe DatabaseManager
from database import db_manager 
from config import Config

app = Flask(__name__)
app.config.from_object(Config)
socketio = SocketIO(app, cors_allowed_origins="*")

# Variáveis globais para controle
current_count = 0
current_shift = None
current_shift_key = None

def get_current_shift():
    """Determina o turno atual baseado no horário"""
    now = datetime.now()
    current_time = now.time()
    
    # Turno 1: 6h às 16h
    if time(6, 0) <= current_time < time(16, 0):
        return "Turno 1 (06:00 - 16:00 h)", now.strftime('%Y-%m-%d')
    
    # Turno 2: 22h às 6h (do dia seguinte)
    elif current_time >= time(22, 0) or current_time < time(6, 0):
        # Se for após 22h, o turno vai até 6h do dia seguinte
        if current_time >= time(22, 0):
            shift_date = now.strftime('%Y-%m-%d')
        else:
            # Se for antes das 6h, é continuação do turno que começou ontem
            shift_date = (now.replace(hour=0, minute=0, second=0, microsecond=0) - 
                          timedelta(days=1)).strftime('%Y-%m-%d')
        return "Turno 2 (22:00 - 06:00 h)", shift_date
    
    # Fora dos turnos
    else:
        return None, None

def initialize_database():
    """Inicializa conexão com banco de dados"""
    if db_manager.connect():
        if db_manager.create_tables():
            print("[OK] Banco de dados inicializado com sucesso")
            return True
    else:
        print("[ERRO] Erro ao conectar com banco de dados")
        return False

def check_shift_change():
    """Verifica se houve mudança de turno e salva o anterior no banco"""
    global current_count, current_shift, current_shift_key
    
    new_shift_name, shift_date = get_current_shift()

    # Extrai o nome e a data do turno anterior a partir da chave atual
    old_shift_name = None
    old_shift_date = None
    if current_shift_key is not None and " - " in current_shift_key:
        # Usa rsplit para separar apenas a última ocorrência (antes da data)
        old_shift_parts = current_shift_key.rsplit(" - ", 1)
        if len(old_shift_parts) == 2:
            old_shift_name = old_shift_parts[0]
            old_shift_date = old_shift_parts[1]

    # Caso estejamos fora do horário de turnos, finalize qualquer turno ativo pendente
    if new_shift_name is None:
        if old_shift_name and old_shift_date:
            db_manager.finish_shift(old_shift_name, old_shift_date)
            print(f"🕘 Fora do horário de turnos. Turno '{current_shift_key}' finalizado.")
        current_shift = None
        current_shift_key = None
        current_count = 0
        return

    # Chave para identificar o turno atual no DB
    shift_key_for_db = f"{new_shift_name} - {shift_date}"

    # Se mudou de turno, finalize o anterior (mesmo que contador seja 0) e resete contador local
    if current_shift_key is not None and current_shift_key != shift_key_for_db:
        if old_shift_name and old_shift_date:
            db_manager.finish_shift(old_shift_name, old_shift_date)
            print(f"🔄 Mudança de turno detectada. Turno anterior '{current_shift_key}' finalizado.")
        current_count = 0

    # Se entramos em um novo turno (ou no início da aplicação), carregue o contador do banco
    if current_shift_key != shift_key_for_db:
        current_count = db_manager.get_current_shift_count(new_shift_name, shift_date)
        print(f"📊 Contador carregado do banco para '{new_shift_name} - {shift_date}': {current_count}")

    # Atualiza variáveis globais para refletir estado atual
    current_shift = new_shift_name
    current_shift_key = shift_key_for_db

@app.route('/')
def index():
    # Garante que o turno atual e a contagem estão corretos ao carregar a página
    check_shift_change()
    history = db_manager.get_shift_history(10) # Busca os últimos 10 dias
    return render_template('index.html',
                           current_count=current_count,
                           current_shift=current_shift,
                           history=history)

@socketio.on('request_initial_data')
def handle_initial_data():
    """Envia dados iniciais quando cliente se conecta"""
    check_shift_change()
    history = db_manager.get_shift_history(10) # Busca os últimos 10 dias
    
    socketio.emit('status', {
        'count': current_count,
        'current_shift': current_shift,
        'history': history,
        'timestamp': datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    })

@app.route('/update', methods=['GET'])
def update():
    global current_count
    
    # Espera receber counter=<valor> via GET
    counter_value = request.args.get('counter', type=int)
    
    # Log detalhado da requisição
    timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    print(f"[{timestamp}] Recebido: counter={counter_value}")
    
    # Verifica mudança de turno (a não ser que ignore seja habilitado)
    if not app.config.get('IGNORE_SHIFT_CHECK', False):
        check_shift_change()
    
    # Log do estado atual
    if current_shift is None:
        print(f"[{timestamp}] FORA DO TURNO")
    else:
        print(f"[{timestamp}] Turno ativo: {current_shift_key}")
    
    # Atualiza contador quando recebe valor válido
    if counter_value is not None and counter_value > 0:
        # Extrai nome e data do turno para o banco
        if current_shift_key and " - " in current_shift_key and current_shift is not None:
            # Separa somente a última ocorrência do separador para obter a data corretamente
            shift_parts = current_shift_key.rsplit(" - ", 1)
            shift_name = shift_parts[0]
            shift_date = shift_parts[1]
            
            # Atualiza contador no banco de dados
            # Se o contador recebido for maior que o atual, atualiza
            if counter_value > current_count:
                # Calcula quantos incrementos foram feitos
                increments = counter_value - current_count
                new_count = current_count
                
                # Incrementa no banco a quantidade necessária
                for _ in range(increments):
                    new_count = db_manager.increment_shift_count(shift_name, shift_date)
                    if new_count is None:
                        print(f"[{timestamp}] ❌ Erro ao incrementar no banco")
                        break
                
                if new_count is not None:
                    current_count = new_count
                    print(f"[{timestamp}] ✅ CONTADOR ATUALIZADO NO BANCO: {current_count}")
                else:
                    # Fallback: atualiza memória para refletir imediatamente no frontend
                    current_count = counter_value
                    print(f"[{timestamp}] ⚠️  Falha no DB, contador atualizado em memória: {current_count}")
            else:
                print(f"[{timestamp}] ℹ️  Contador recebido ({counter_value}) não é maior que atual ({current_count})")
        else:
            # Fora de turno ou sem chave válida: ainda assim atualiza o valor em memória para refletir no frontend
            if counter_value > current_count:
                current_count = counter_value
                print(f"[{timestamp}] ⚠️  Atualizado apenas em memória (fora do turno/sem chave). count={current_count}")
            else:
                print(f"[{timestamp}] ℹ️  (memória) Contador recebido ({counter_value}) não é maior que atual ({current_count})")
        
    elif counter_value is not None and counter_value <= 0:
        print(f"[{timestamp}] ❌ Valor inválido de contador: {counter_value}")
    elif counter_value is None:
        print(f"[{timestamp}] ❌ Dados inválidos recebidos")
    
    # Obtém histórico atualizado do banco
    history = db_manager.get_shift_history(10) # Busca os últimos 10 dias
    
    # Emite para todos os clientes conectados
    socketio.emit('status', {
        'count': current_count,
        'current_shift': current_shift,
        'history': history,
        'timestamp': timestamp
    })
    
    print(f"[{timestamp}] Status enviado para clientes: count={current_count}\n")
    # Retorna informações úteis na resposta para diagnóstico rápido
    return {
        'ok': True,
        'count': current_count,
        'received': counter_value,
        'shift': current_shift,
        'ignore_shift_check': app.config.get('IGNORE_SHIFT_CHECK', False)
    }, 200

@app.route('/admin/meta', methods=['POST'])
def set_meta():
    data = request.get_json(silent=True) or {}
    turno_nome = data.get('turno_nome')
    meta_turno = data.get('meta_turno')
    meta_dia = data.get('meta_dia')

    # A data do turno agora é sempre a data atual, simplificando o frontend
    _, shift_date_str = get_current_shift()
    data_turno = shift_date_str or datetime.now().strftime('%Y-%m-%d')

    if not turno_nome or meta_turno is None:
        return jsonify({'ok': False, 'error': 'turno_nome e meta_turno são obrigatórios'}), 400
    ok = db_manager.set_goal_for_shift(turno_nome, data_turno, int(meta_turno), int(meta_dia) if meta_dia is not None else None)
    return jsonify({'ok': ok}), 200 if ok else 500

@app.route('/admin/perda', methods=['POST'])
def add_perda():
    data = request.get_json(silent=True) or {}
    turno_nome = data.get('turno_nome')
    data_turno = data.get('data_turno')  # formato YYYY-MM-DD
    quantidade = data.get('quantidade')
    motivo = data.get('motivo', 'Perda registrada manualmente')  # motivo padrão se não fornecido
    data_evento = data.get('data_evento')  # opcional, timestamp ISO
    
    # Log para debug
    print(f"[DEBUG] Recebendo perda: turno_nome={turno_nome}, data_turno={data_turno}, quantidade={quantidade}, motivo={motivo}")
    
    if not turno_nome or not data_turno or quantidade is None:
        error_msg = 'turno_nome, data_turno e quantidade são obrigatórios'
        print(f"[DEBUG] Erro de validação: {error_msg}")
        return jsonify({'ok': False, 'error': error_msg}), 400
    
    
    ok = db_manager.insert_loss(turno_nome, data_turno, int(quantidade), motivo, data_evento)
    print(f"[DEBUG] Resultado da inserção: {ok}")
    
    if ok:
        # Força sincronização do histórico após inserir perda
        try:
            history = db_manager.get_shift_history(10) # Busca os últimos 10 dias
            socketio.emit('status', {
                'count': current_count,
                'current_shift': current_shift,
                'history': history,
                'timestamp': datetime.now().strftime('%Y-%m-%d %H:%M:%S')
            })
            print("[DEBUG] Histórico sincronizado após inserção de perda")
        except Exception as e:
            print(f"[DEBUG] Erro ao sincronizar histórico: {e}")
    
    return jsonify({'ok': ok}), 200 if ok else 500

@app.route('/admin/perdas_historico', methods=['GET'])
def get_losses_history():
    """Retorna o histórico de perdas das últimas 24 horas."""
    try:
        losses = db_manager.get_losses_history(24)  # últimas 24 horas
        return jsonify({'ok': True, 'data': losses})
    except Exception as e:
        print(f"[ERRO] Erro ao obter histórico de perdas: {e}")
        return jsonify({'ok': False, 'error': str(e)}), 500

@app.route('/metrics/shift', methods=['GET'])
def metrics_shift():
    turno_nome = request.args.get('turno_nome')
    data_turno = request.args.get('data_turno')  # YYYY-MM-DD
    
    if not turno_nome or not data_turno:
        return jsonify({'ok': False, 'error': 'turno_nome e data_turno são obrigatórios'}), 400
    
    metrics = db_manager.get_shift_metrics(turno_nome, data_turno)
    
    if metrics is None:
        return jsonify({'ok': False, 'error': 'Turno não encontrado'}), 404
    return jsonify({'ok': True, 'metrics': metrics})

@app.route('/metrics/aggregate', methods=['GET'])
def metrics_aggregate():
    period = request.args.get('period', 'day')  # day|week|month|year
    aggregates = db_manager.get_period_aggregates(period)
    return jsonify({'ok': True, 'period': period, 'data': aggregates})

@app.route('/metrics/perdas_distribuicao', methods=['GET'])
def perdas_distribuicao():
    period = request.args.get('period', 'day')
    data = db_manager.get_losses_distribution(period)
    return jsonify({'ok': True, 'period': period, 'data': data})

@app.route('/metrics/ranking_turnos', methods=['GET'])
def ranking_turnos():
    data = db_manager.get_shifts_efficiency_ranking()
    return jsonify({'ok': True, 'data': data})

@app.route('/metrics/efficiency_series', methods=['GET'])
def efficiency_series():
    days = request.args.get('days', default=7, type=int)
    data = db_manager.get_daily_efficiency_series(days)
    return jsonify({'ok': True, 'days': days, 'data': data})

@app.route('/debug_status', methods=['GET'])
def debug_status():
    """Rota de diagnóstico para verificar estado atual do servidor."""
    check_shift_change()
    return {
        'count': current_count,
        'shift': current_shift,
        'shift_key': current_shift_key,
        'ignore_shift_check': app.config.get('IGNORE_SHIFT_CHECK', False)
    }, 200

@app.route('/debug_shifts', methods=['GET'])
def debug_shifts():
    """Rota de debug para listar todos os turnos e perdas."""
    try:
        # Lista todos os turnos
        db_manager.cursor.execute("SELECT id, turno_nome, data_turno, contador FROM shifts ORDER BY data_turno DESC, id DESC LIMIT 20")
        shifts = []
        for row in db_manager.cursor.fetchall():
            shifts.append({
                'id': row[0],
                'turno_nome': row[1],
                'data_turno': str(row[2]),
                'contador': row[3]
            })
        
        # Lista todas as perdas
        db_manager.cursor.execute("""
            SELECT p.id, p.quantidade, p.motivo, p.data_evento, s.turno_nome, s.data_turno 
            FROM perdas p 
            JOIN shifts s ON p.shift_id = s.id 
            ORDER BY p.data_evento DESC LIMIT 20
        """)
        losses = []
        for row in db_manager.cursor.fetchall():
            losses.append({
                'id': row[0],
                'quantidade': row[1],
                'motivo': row[2],
                'data_evento': str(row[3]) if row[3] else None,
                'turno_nome': row[4],
                'data_turno': str(row[5])
            })
        
        return jsonify({
            'shifts': shifts,
            'losses': losses
        })
    except Exception as e:
        return jsonify({'error': str(e)}), 500

@app.route('/sync_history', methods=['POST'])
def sync_history():
    """Força sincronização do histórico com o banco de dados."""
    try:
        # Obtém histórico atualizado do banco
        history = db_manager.get_shift_history(10) # Busca os últimos 10 dias
        
        # Emite atualização para todos os clientes conectados
        socketio.emit('status', {
            'count': current_count,
            'current_shift': current_shift,
            'history': history,
            'timestamp': datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        })
        
        return jsonify({'ok': True, 'message': 'Histórico sincronizado'})
    except Exception as e:
        return jsonify({'ok': False, 'error': str(e)}), 500

@app.route('/metrics/performance', methods=['GET'])
def get_performance_data():
    """Retorna dados de performance agregados por período."""
    period = request.args.get('period', 'day')
    mode = request.args.get('mode', 'total')
    data = db_manager.get_performance_data(period, mode)
    return jsonify({'ok': True, 'period': period, 'data': data})

# (Opcional) mantém suas rotas existentes de CLICK/SOLTO
@app.route('/CLICK', methods=['GET','POST'])
def click():
    socketio.emit('command', {'action': 'click'})
    return 'Click command sent', 200

@app.route('/SOLTO', methods=['GET','POST'])
def solto():
    socketio.emit('command', {'action': 'solto'})
    return 'solto command sent', 200

if __name__ == '__main__':
    print("=" * 60)
    print("🚀 SISTEMA DE CONTADOR POR TURNOS INICIADO")
    print("=" * 60)
    print("📅 HORÁRIOS DOS TURNOS:")
    print("    • Turno 1: 06:00 às 16:00")
    print("    • Turno 2: 22:00 às 06:00 (do dia seguinte)")
    print("    • Fora dos turnos: 16:00 às 22:00")
    print("=" * 60)
    
    # Inicializa banco de dados
    if not initialize_database():
        print("[ERRO] ERRO: Não foi possível inicializar o banco de dados")
        print("[INFO] Verifique as configurações no arquivo .env")
        print("[INFO] Certifique-se que o PostgreSQL está rodando")
        exit(1)
    
    print(f"🌐 Servidor rodando em: http://{app.config['SERVER_HOST']}:{app.config['SERVER_PORT']}")
    print(f"🗄️  Banco de dados: {db_manager.config.DB_NAME}")
    print("=" * 60)
    
    # Verifica turno atual ao iniciar
    check_shift_change()
    if current_shift:
        print(f"✅ Turno atual: {current_shift}")
        print(f"📊 Contador atual: {current_count}")
    else:
        print("⏰ Atualmente FORA do horário de turnos")
    
    # Mostra turnos ativos no banco
    active_shifts = db_manager.get_current_shifts()
    if active_shifts:
        print("📋 Turnos ativos no banco (ainda não finalizados):")
        for shift in active_shifts:
            print(f"    • {shift['turno_nome']} ({shift['data_turno']}): {shift['contador']} acionamentos (início: {shift['inicio_turno']})")
    else:
        print("📋 Não há turnos ativos registrados no banco.")
    
    print("=" * 60)
    
    try:
        ssl_ctx = None
        if app.config.get('SSL_ENABLED'):
            cert_file = app.config.get('SSL_CERT_FILE')
            key_file = app.config.get('SSL_KEY_FILE')
            if os.path.exists(cert_file) and os.path.exists(key_file):
                ssl_ctx = (cert_file, key_file)
                print(f"🔒 HTTPS habilitado com cert: {cert_file}")
            else:
                print("⚠️  SSL_ENABLED está ativo, mas cert/key não foram encontrados. Iniciando em HTTP.")

        socketio.run(app,
                     host=app.config['SERVER_HOST'],
                     port=app.config['SERVER_PORT'],
                     allow_unsafe_werkzeug=True,
                     ssl_context=ssl_ctx)
    except KeyboardInterrupt:
        print("\n🛑 Servidor interrompido pelo usuário")
    finally:
        db_manager.disconnect()
        print("👋 Desconectando do banco de dados...")
