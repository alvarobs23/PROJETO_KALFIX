# database.py
import psycopg2
from psycopg2 import sql
import os
from datetime import datetime

# Importa as configurações da aplicação
from config import Config

class DatabaseManager:
    def __init__(self):
        self.config = Config()
        self.conn = None
        self.cursor = None

    def connect(self):
        """Tenta estabelecer uma conexão com o banco de dados PostgreSQL."""
        if not self.config.DATABASE_URL:
            print("Erro: DATABASE_URL não definida nas variáveis de ambiente.")
            return False
        try:
            self.conn = psycopg2.connect(self.config.DATABASE_URL)
            self.cursor = self.conn.cursor()
            print("[OK] Conectado ao banco de dados PostgreSQL.")
            return True
        except psycopg2.OperationalError as e:
            print(f"[ERRO] Erro ao conectar ao banco de dados: {e}")
            return False

    def disconnect(self):
        """Fecha a conexão com o banco de dados."""
        if self.cursor:
            self.cursor.close()
        if self.conn:
            self.conn.close()
            print("[INFO] Conexão com o banco de dados fechada.")

    def reset_connection(self):
        """Reseta a conexão com o banco de dados em caso de erro de transação."""
        try:
            if self.conn:
                self.conn.rollback()
                print("[INFO] Transação resetada com rollback.")
        except Exception as e:
            print(f"[INFO] Erro ao resetar transação: {e}")
            # Se não conseguir resetar, reconecta
            self.disconnect()
            self.connect()

    def create_tables(self):
        """Cria as tabelas necessárias no banco de dados se elas não existirem."""
        try:
            # Tabela para armazenar as contagens dos turnos
            self.cursor.execute("""
                CREATE TABLE IF NOT EXISTS shifts (
                    id SERIAL PRIMARY KEY,
                    turno_nome VARCHAR(255) NOT NULL,
                    data_turno DATE NOT NULL,
                    contador INTEGER DEFAULT 0,
                    perdas INTEGER DEFAULT 0,
                    inicio_turno TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    fim_turno TIMESTAMP NULL,
                    UNIQUE(turno_nome, data_turno)
                );
            """)
            self.conn.commit()
            print("[OK] Tabela 'shifts' verificada/criada com sucesso.")
            
            # Adiciona coluna de perdas se não existir (para tabelas existentes)
            try:
                self.cursor.execute("ALTER TABLE shifts ADD COLUMN IF NOT EXISTS perdas INTEGER DEFAULT 0;")
                self.conn.commit()
                print("[OK] Coluna 'perdas' adicionada/verificada na tabela 'shifts'.")
            except Exception as e:
                print(f"[INFO] Coluna 'perdas' já existe ou erro ao adicionar: {e}")

            # Tabela de metas por turno e por dia
            self.cursor.execute("""
                CREATE TABLE IF NOT EXISTS metas (
                    id SERIAL PRIMARY KEY,
                    shift_id INTEGER NOT NULL REFERENCES shifts(id) ON DELETE CASCADE,
                    meta_turno INTEGER NOT NULL,
                    meta_dia INTEGER NULL,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    UNIQUE(shift_id)
                );
            """)
            self.conn.commit()
            print("[OK] Tabela 'metas' verificada/criada com sucesso.")

            # Tabela de perdas (quantidade, motivo, data_evento) ligada a turno
            self.cursor.execute("""
                CREATE TABLE IF NOT EXISTS perdas (
                    id SERIAL PRIMARY KEY,
                    shift_id INTEGER NOT NULL REFERENCES shifts(id) ON DELETE CASCADE,
                    quantidade INTEGER NOT NULL CHECK (quantidade >= 0),
                    motivo VARCHAR(255) NOT NULL,
                    data_evento TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                );
            """)
            self.conn.commit()
            print("[OK] Tabela 'perdas' verificada/criada com sucesso.")

            # Índices para performance em relatórios
            self.cursor.execute("""
                CREATE INDEX IF NOT EXISTS idx_shifts_data ON shifts(data_turno);
                CREATE INDEX IF NOT EXISTS idx_perdas_shift ON perdas(shift_id);
                CREATE INDEX IF NOT EXISTS idx_perdas_data ON perdas(data_evento);
            """)
            self.conn.commit()
            return True
        except Exception as e:
            print(f"[ERRO] Erro ao criar tabelas: {e}")
            self.conn.rollback() # Reverte qualquer transação em caso de erro
            return False

    def get_current_shift_count(self, turno_nome, data_turno):
        """Obtém a contagem atual para um turno específico ou 0 se não existir."""
        try:
            self.cursor.execute(
                "SELECT contador FROM shifts WHERE turno_nome = %s AND data_turno = %s",
                (turno_nome, data_turno)
            )
            result = self.cursor.fetchone()
            if result:
                return result[0]
            else:
                # Se o turno não existe, insere-o com contador 0 e retorna 0
                self.cursor.execute(
                    "INSERT INTO shifts (turno_nome, data_turno, contador) VALUES (%s, %s, %s) RETURNING contador",
                    (turno_nome, data_turno, 0)
                )
                self.conn.commit()
                return 0
        except Exception as e:
            print(f"[ERRO] Erro ao obter/inicializar contador do turno: {e}")
            self.conn.rollback()
            return 0 # Retorna 0 em caso de erro para evitar problemas

    def increment_shift_count(self, turno_nome, data_turno):
        """Incrementa o contador para um turno específico."""
        try:
            self.cursor.execute(
                """
                INSERT INTO shifts (turno_nome, data_turno, contador)
                VALUES (%s, %s, 1)
                ON CONFLICT (turno_nome, data_turno) DO UPDATE
                SET contador = shifts.contador + 1
                RETURNING contador;
                """,
                (turno_nome, data_turno)
            )
            new_count = self.cursor.fetchone()[0]
            self.conn.commit()
            return new_count
        except Exception as e:
            print(f"[ERRO] Erro ao incrementar contador do turno: {e}")
            self.conn.rollback()
            return None # Retorna None em caso de erro

    def finish_shift(self, turno_nome, data_turno):
        """Marca um turno como finalizado no banco de dados."""
        try:
            self.cursor.execute(
                "UPDATE shifts SET fim_turno = NOW() WHERE turno_nome = %s AND data_turno = %s",
                (turno_nome, data_turno)
            )
            self.conn.commit()
            print(f"[OK] Turno '{turno_nome}' do dia '{data_turno}' finalizado no banco.")
            return True
        except Exception as e:
            print(f"[ERRO] Erro ao finalizar turno: {e}")
            self.conn.rollback()
            return False

    def get_shift_history(self, days=10):
        """Obtém o histórico completo dos turnos do banco de dados."""
        try:
            self.cursor.execute(
                """
                WITH date_series AS (
                    -- Gera uma série de datas para os últimos N dias
                    SELECT generate_series(
                        CURRENT_DATE - (%s - 1) * INTERVAL '1 day',
                        CURRENT_DATE,
                        '1 day'::interval
                    )::date AS report_date
                ),
                shift_names AS (
                    -- Define os nomes dos turnos que queremos na grade
                    SELECT unnest(ARRAY['Turno 1 (06:00 - 16:00 h)', 'Turno 2 (22:00 - 06:00 h)']) AS turno_nome
                ),
                full_grid AS (
                    -- Cria uma grade completa com todas as datas e turnos
                    SELECT
                        d.report_date,
                        s.turno_nome
                    FROM date_series d
                    CROSS JOIN shift_names s
                )
                -- Junta a grade completa com os dados existentes na tabela de turnos
                SELECT
                    g.turno_nome,
                    g.report_date AS data_turno,
                    COALESCE(s.contador, 0) AS contador,
                    COALESCE(s.perdas, 0) AS perdas,
                    s.inicio_turno,
                    s.fim_turno
                FROM full_grid g
                LEFT JOIN shifts s ON g.report_date = s.data_turno AND g.turno_nome = s.turno_nome
                ORDER BY g.report_date DESC, g.turno_nome ASC;
                """,
                (days,)
            )
            history = []
            for row in self.cursor.fetchall():
                history.append({
                    'turno_nome': row[0],
                    'data_turno': row[1].strftime('%d/%m/%Y'), # Formato DD/MM/YYYY
                    'contador': row[2],
                    'perdas': row[3] or 0,
                    'inicio_turno': row[4].strftime('%d/%m/%Y - %H:%M') if row[4] else None,
                    'fim_turno': row[5].strftime('%d/%m/%Y - %H:%M') if row[5] else None
                })
            return history
        except Exception as e:
            print(f"[ERRO] Erro ao obter histórico de turnos: {e}")
            # Tenta resetar a conexão se houver erro de transação
            if "transação atual foi interrompida" in str(e):
                print("[INFO] Tentando resetar conexão devido a erro de transação...")
                self.reset_connection()
            return []

    def _get_or_create_shift(self, turno_nome, data_turno):
        """Obtém id do turno ou cria se não existir, retornando (id, contador)."""
        try:
            print(f"[DEBUG] _get_or_create_shift: turno_nome={turno_nome}, data_turno={data_turno}")
            
            # Primeiro, tenta buscar o turno existente
            self.cursor.execute(
                "SELECT id, contador FROM shifts WHERE turno_nome = %s AND data_turno = %s",
                (turno_nome, data_turno)
            )
            row = self.cursor.fetchone()
            print(f"[DEBUG] _get_or_create_shift: row encontrada={row}")
            if row:
                print(f"[DEBUG] _get_or_create_shift: Turno existente encontrado: id={row[0]}, contador={row[1]}")
                return row[0], row[1]
            
            # Se não existe, cria um novo turno com timestamp atual
            print(f"[DEBUG] _get_or_create_shift: Criando novo turno para {turno_nome} - {data_turno}")
            self.cursor.execute(
                "INSERT INTO shifts (turno_nome, data_turno, contador, inicio_turno) VALUES (%s, %s, %s, NOW()) RETURNING id, contador",
                (turno_nome, data_turno, 0)
            )
            res = self.cursor.fetchone()
            print(f"[DEBUG] _get_or_create_shift: Resultado da inserção={res}")
            if res:
                self.conn.commit()
                print(f"[DEBUG] _get_or_create_shift: Novo turno criado: id={res[0]}, contador={res[1]}")
                return res[0], res[1]
            else:
                self.conn.rollback()
                print("[DEBUG] _get_or_create_shift: Falha na inserção, retornando None")
                return None, 0
        except Exception as e:
            print(f"[ERRO] _get_or_create_shift: {e}")
            try:
                self.conn.rollback()
            except:
                pass  # Ignora erro de rollback se já foi feito
            return None, 0

    def set_goal_for_shift(self, turno_nome, data_turno, meta_turno, meta_dia=None):
        """Define meta do turno (e opcional meta diária) para um turno específico."""
        shift_id, _ = self._get_or_create_shift(turno_nome, data_turno)
        if shift_id is None:
            print(f"[ERRO] set_goal_for_shift: Não foi possível obter/criar shift_id para {turno_nome} - {data_turno}")
            return False
        try:
            self.cursor.execute(
                """
                INSERT INTO metas (shift_id, meta_turno, meta_dia)
                VALUES (%s, %s, %s)
                ON CONFLICT (shift_id)
                DO UPDATE SET meta_turno = EXCLUDED.meta_turno, meta_dia = EXCLUDED.meta_dia
                """,
                (shift_id, meta_turno, meta_dia)
            )
            self.conn.commit()
            return True
        except Exception as e:
            print(f"[ERRO] set_goal_for_shift: {e}")
            self.conn.rollback()
            return False

    def insert_loss(self, turno_nome, data_turno, quantidade, motivo, data_evento=None):
        """Adiciona perdas diretamente na tabela shifts."""
        try:
            print(f"[DEBUG] insert_loss: turno_nome={turno_nome}, data_turno={data_turno}, quantidade={quantidade}")
            
            # Atualiza ou cria o turno com as perdas
            self.cursor.execute(
                """
                INSERT INTO shifts (turno_nome, data_turno, contador, perdas, inicio_turno)
                VALUES (%s, %s, 0, %s, NOW())
                ON CONFLICT (turno_nome, data_turno) 
                DO UPDATE SET perdas = shifts.perdas + %s
                RETURNING id, perdas;
                """,
                (turno_nome, data_turno, quantidade, quantidade)
            )
            
            result = self.cursor.fetchone()
            if result:
                shift_id, total_perdas = result
                self.conn.commit()
                print(f"[DEBUG] Perdas adicionadas com sucesso: shift_id={shift_id}, quantidade={quantidade}, total_perdas={total_perdas}")
                return True
            else:
                self.conn.rollback()
                print("[DEBUG] Falha ao adicionar perdas")
                return False
                
        except Exception as e:
            print(f"[ERRO] insert_loss: {e}")
            try:
                self.conn.rollback()
            except:
                pass  # Ignora erro de rollback se já foi feito
            return False

    def get_shift_metrics(self, turno_nome, data_turno):
        """Calcula métricas do turno: bruto, perdas, liquida, metas, taxas, produtividade."""
        try:
            # Dados do turno incluindo perdas
            self.cursor.execute(
                "SELECT id, contador, perdas, inicio_turno, fim_turno FROM shifts WHERE turno_nome = %s AND data_turno = %s",
                (turno_nome, data_turno)
            )
            row = self.cursor.fetchone()
            if not row:
                return None
            
            # Verifica se a row tem o número correto de elementos
            if len(row) < 5:
                return None
                
            shift_id, contador, perdas, inicio_turno, fim_turno = row

            # Meta do turno
            self.cursor.execute(
                "SELECT meta_turno, meta_dia FROM metas WHERE shift_id = %s;",
                (int(shift_id),)
            )
            meta_row = self.cursor.fetchone()
            meta_turno = meta_row[0] if meta_row and len(meta_row) > 0 else None
            meta_dia = meta_row[1] if meta_row and len(meta_row) > 1 else None
            
            producao_bruta = contador
            producao_liquida = max(producao_bruta - perdas, 0)
            taxa_perdas = (perdas / producao_bruta * 100.0) if producao_bruta > 0 else 0.0
            eficiencia = (producao_bruta / meta_turno * 100.0) if (meta_turno and meta_turno > 0) else None

            # Produtividade por hora
            from datetime import datetime as _dt
            inicio = inicio_turno or _dt.now()
            fim = fim_turno or _dt.now()
            duracao_horas = max((fim - inicio).total_seconds() / 3600.0, 0.0001)
            produtividade_hora = producao_bruta / duracao_horas

            return {
                'producao_bruta': producao_bruta,
                'perdas': perdas,
                'producao_liquida': producao_liquida,
                'taxa_perdas': taxa_perdas,
                'eficiencia': eficiencia,
                'produtividade_hora': produtividade_hora,
                'meta_turno': meta_turno,
                'meta_dia': meta_dia
            }
        except Exception as e:
            print(f"[ERRO] get_shift_metrics: {e}")
            # Tenta resetar a conexão se houver erro de transação
            if "transação atual foi interrompida" in str(e):
                print("[INFO] Tentando resetar conexão devido a erro de transação...")
                self.reset_connection()
            return None

    def get_period_aggregates(self, period: str, reference_date=None):
        """Retorna agregados diário, semanal, mensal ou anual: bruto, perdas, liquida por data."""
        from datetime import datetime as _dt, timedelta as _td
        try:
            if reference_date is None:
                reference_date = _dt.now().date()

            # Define intervalo
            if period == 'day':
                start_date = reference_date
                end_date = reference_date
            elif period == 'week':
                start_date = reference_date - _td(days=reference_date.weekday())
                end_date = start_date + _td(days=6)
            elif period == 'month':
                start_date = reference_date.replace(day=1)
                # próximo mês - 1 dia
                if start_date.month == 12:
                    end_date = start_date.replace(year=start_date.year+1, month=1, day=1) - _td(days=1)
                else:
                    end_date = start_date.replace(month=start_date.month+1, day=1) - _td(days=1)
            elif period == 'year':
                start_date = reference_date.replace(month=1, day=1)
                end_date = reference_date.replace(month=12, day=31)
            else:
                return []

            # Agregado por dia dentro do intervalo
            self.cursor.execute(
                """
                SELECT
                    data_turno::date as dia,
                    SUM(contador) as producao_bruta,
                    SUM(perdas) as perdas,
                    SUM(contador) - SUM(perdas) as producao_liquida
                FROM shifts
                WHERE data_turno BETWEEN %s AND %s
                GROUP BY dia
                ORDER BY dia ASC;
                """,
                (start_date, end_date)
            )
            rows = self.cursor.fetchall()
            result = []
            for r in rows:
                # Trata tanto objetos date quanto strings
                if hasattr(r[0], 'strftime'):
                    data_str = r[0].strftime('%Y-%m-%d')
                else:
                    data_str = str(r[0])
                
                result.append({
                    'data': data_str,
                    'producao_bruta': int(r[1] or 0),
                    'perdas': int(r[2] or 0),
                    'producao_liquida': int(r[3] or 0)
                })
            return result
        except Exception as e:
            print(f"[ERRO] get_period_aggregates: {e}")
            # Tenta resetar a conexão se houver erro de transação
            if "transação atual foi interrompida" in str(e):
                print("[INFO] Tentando resetar conexão devido a erro de transação...")
                self.reset_connection()
            return []

    def get_losses_distribution(self, period: str, reference_date=None):
        """Distribuição das perdas por motivo no período especificado."""
        from datetime import datetime as _dt, timedelta as _td
        try:
            if reference_date is None:
                reference_date = _dt.now().date()
            if period == 'day':
                start_date = _dt.combine(reference_date, _dt.min.time())
                end_date = _dt.combine(reference_date, _dt.max.time())
            elif period == 'week':
                start_date = _dt.combine(reference_date - _td(days=reference_date.weekday()), _dt.min.time())
                end_date = start_date + _td(days=7)
            elif period == 'month':
                start_date = _dt.combine(reference_date.replace(day=1), _dt.min.time())
                if start_date.month == 12:
                    end_date = _dt.combine(start_date.replace(year=start_date.year+1, month=1, day=1) - _td(days=1), _dt.max.time())
                else:
                    end_date = _dt.combine(start_date.replace(month=start_date.month+1, day=1) - _td(days=1), _dt.max.time())
            elif period == 'year':
                start_date = _dt.combine(reference_date.replace(month=1, day=1), _dt.min.time())
                end_date = _dt.combine(reference_date.replace(month=12, day=31), _dt.max.time())
            else:
                return []

            self.cursor.execute(
                """
                SELECT motivo, COALESCE(SUM(quantidade),0) as total
                FROM perdas
                WHERE data_evento BETWEEN %s AND %s
                GROUP BY motivo
                ORDER BY total DESC
                """,
                (start_date, end_date)
            )
            rows = self.cursor.fetchall()
            return [{'motivo': r[0], 'total': int(r[1])} for r in rows]
        except Exception as e:
            print(f"[ERRO] get_losses_distribution: {e}")
            return []

    def get_shifts_efficiency_ranking(self, reference_date=None):
        """Ranking dos turnos por eficiência do dia informado (ou do dia atual)."""
        from datetime import datetime as _dt
        try:
            if reference_date is None:
                reference_date = _dt.now().date()
            self.cursor.execute(
                """
                WITH perdas_sum AS (
                    SELECT shift_id, SUM(quantidade) as total_perdas
                    FROM perdas
                    GROUP BY shift_id
                )
                SELECT
                    s.turno_nome, s.data_turno, s.contador, s.perdas, m.meta_turno
                FROM shifts s
                LEFT JOIN metas m ON m.shift_id = s.id
                WHERE s.data_turno = %s;
                """,
                (reference_date,)
            )
            rows = self.cursor.fetchall()
            ranking = []
            for row in rows:
                turno_nome, data_turno, bruto, perdas, meta_turno = row
                liquida = max((bruto or 0) - (perdas or 0), 0) # Mantém a líquida para informação
                efic = (bruto / meta_turno * 100.0) if meta_turno and meta_turno > 0 else None
                # Trata data_turno que pode ser date ou string
                if hasattr(data_turno, 'strftime'):
                    data_str = data_turno.strftime('%Y-%m-%d')
                else:
                    data_str = str(data_turno)
                
                ranking.append({
                    'turno_nome': turno_nome,
                    'data_turno': data_str,
                    'eficiencia': efic,
                    'producao_bruta': int(bruto or 0),
                    'perdas': int(perdas or 0),
                    'producao_liquida': int(liquida)
                })
            # Ordena por eficiência desc, None por último
            ranking.sort(key=lambda x: (-x['eficiencia'] if x['eficiencia'] is not None else float('-inf')))
            return ranking
        except Exception as e:
            print(f"[ERRO] get_shifts_efficiency_ranking: {e}")
            # Tenta resetar a conexão se houver erro de transação
            if "transação atual foi interrompida" in str(e):
                print("[INFO] Tentando resetar conexão devido a erro de transação...")
                self.reset_connection()
            return []

    def get_daily_efficiency_series(self, days: int = 7):
        """Retorna série de eficiência diária dos últimos N dias: (liquida/MetaDia)*100.
        Considera meta_dia se disponível; senão soma meta_turno dos turnos do dia.
        """
        try:
            # Agrega por data: soma liquida e soma meta (preferindo meta_dia quando presente)
            self.cursor.execute(
                """
                WITH base AS (
                    SELECT s.id as shift_id, s.data_turno::date as dia, s.contador as bruto
                    FROM shifts s
                    WHERE s.data_turno >= CURRENT_DATE - %s::int
                ), perdas_sum AS (
                    SELECT p.shift_id, COALESCE(SUM(p.quantidade),0) as perdas
                    FROM perdas p
                    GROUP BY p.shift_id
                ), metas_join AS (
                    SELECT m.shift_id, m.meta_turno, m.meta_dia
                    FROM metas m
                )
                SELECT b.dia,
                       SUM(b.bruto - COALESCE(ps.perdas,0)) as liquida,
                       CASE WHEN COUNT(mj.meta_dia) > 0 AND SUM(COALESCE(mj.meta_dia,0)) > 0
                            THEN MAX(mj.meta_dia) -- se meta_dia cadastrada por turno, use a soma via MAX por dia? fallback abaixo
                            ELSE SUM(COALESCE(mj.meta_turno,0))
                       END as meta_total
                FROM base b
                LEFT JOIN perdas_sum ps ON ps.shift_id = b.shift_id
                LEFT JOIN metas_join mj ON mj.shift_id = b.shift_id
                GROUP BY b.dia
                ORDER BY b.dia ASC
                """,
                (days,)
            )
            rows = self.cursor.fetchall()
            series = []
            for dia, liquida, meta_total in rows:
                liquida = int(liquida or 0)
                meta_val = int(meta_total or 0)
                eficiencia = (liquida / meta_val * 100.0) if meta_val > 0 else None
                # Trata dia que pode ser date ou string
                if hasattr(dia, 'strftime'):
                    data_str = dia.strftime('%Y-%m-%d')
                else:
                    data_str = str(dia)
                
                series.append({
                    'data': data_str,
                    'liquida': liquida,
                    'meta': meta_val,
                    'eficiencia': eficiencia
                })
            return series
        except Exception as e:
            print(f"[ERRO] get_daily_efficiency_series: {e}")
            return []
            
    def get_current_shifts(self):
        """Obtém os turnos que ainda não foram finalizados (fim_turno is NULL)."""
        try:
            self.cursor.execute(
                """
                SELECT turno_nome, data_turno, contador, inicio_turno
                FROM shifts
                WHERE fim_turno IS NULL
                ORDER BY data_turno DESC, inicio_turno DESC;
                """
            )
            active_shifts = []
            for row in self.cursor.fetchall():
                active_shifts.append({
                    'turno_nome': row[0],
                    'data_turno': row[1].strftime('%Y-%m-%d'),
                    'contador': row[2],
                    'inicio_turno': row[3].strftime('%Y-%m-%d %H:%M:%S') if row[3] else None
                })
            return active_shifts
        except Exception as e:
            print(f"[ERRO] Erro ao obter turnos ativos: {e}")
            return []

    def get_losses_history(self, hours=24):
        """Obtém o histórico de perdas das últimas N horas."""
        try:
            self.cursor.execute(
                """
                SELECT p.quantidade, p.motivo, p.data_evento, s.turno_nome, s.data_turno
                FROM perdas p
                JOIN shifts s ON p.shift_id = s.id
                WHERE p.data_evento >= NOW() - INTERVAL '%s hours'
                ORDER BY p.data_evento DESC
                LIMIT 50;
                """,
                (hours,)
            )
            losses = []
            for row in self.cursor.fetchall():
                losses.append({
                    'quantidade': row[0],
                    'motivo': row[1],
                    'data_evento': row[2].isoformat() if row[2] else None,
                    'turno_nome': row[3],
                    'data_turno': row[4].strftime('%d/%m/%Y') if row[4] else None
                })
            return losses
        except Exception as e:
            print(f"[ERRO] Erro ao obter histórico de perdas: {e}")
            return []

    def get_performance_data(self, period='day', mode='total'):
        """
        Agrega dados de produção e perdas por período, com filtro opcional por turno.
        mode: 'total', 'turno1', 'turno2', 'ambos'
        """
        try:
            # Valida o período para evitar SQL Injection
            if period not in ['day', 'week', 'month', 'year']:
                period = 'day'

            # Constrói a query dinamicamente
            where_clauses = [sql.SQL("data_turno >= CURRENT_DATE - INTERVAL '1 year'")]
            grouping_fields = []
            select_fields = [
                sql.SQL("date_trunc(%s, data_turno) AS period_start"),
                sql.SQL("SUM(contador) AS total_producao"),
                sql.SQL("SUM(perdas) AS total_perdas")
            ]

            if mode == 'turno1':
                where_clauses.append(sql.SQL("turno_nome = 'Turno 1 (06:00 - 16:00 h)'"))
            elif mode == 'turno2':
                where_clauses.append(sql.SQL("turno_nome = 'Turno 2 (22:00 - 06:00 h)'"))
            elif mode == 'ambos':
                select_fields.insert(1, sql.SQL("turno_nome"))
                grouping_fields.append(sql.SQL("turno_nome"))

            query = sql.SQL("""
                SELECT {select_fields}
                FROM shifts
                WHERE {where_conditions}
                GROUP BY period_start {grouping_fields}
                ORDER BY period_start ASC, {order_by_group};
            """).format(
                select_fields=sql.SQL(', ').join(select_fields),
                where_conditions=sql.SQL(' AND ').join(where_clauses),
                grouping_fields=sql.SQL(', ') + sql.SQL(', ').join(grouping_fields) if grouping_fields else sql.SQL(''),
                order_by_group=sql.SQL(', ').join(grouping_fields) if grouping_fields else sql.SQL("period_start")
            )

            # Usa date_trunc para agrupar por período
            self.cursor.execute(query, (period,))
            
            results = []
            for row in self.cursor.fetchall():
                item = {'period_start': row[0].isoformat() if row[0] else None}
                if mode == 'ambos':
                    item['turno_nome'] = row[1]
                    item['total_producao'] = int(row[2] or 0)
                    item['total_perdas'] = int(row[3] or 0)
                else:
                    item['total_producao'] = int(row[1] or 0)
                    item['total_perdas'] = int(row[2] or 0)
                results.append(item)
            return results
        except Exception as e:
            print(f"[ERRO] Erro ao obter dados de performance ({period}): {e}")
            self.conn.rollback()
            return []

# Cria uma instância global do DatabaseManager
db_manager = DatabaseManager()
