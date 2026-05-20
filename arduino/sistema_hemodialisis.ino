
import streamlit as st
import serial
import json
import os
import time
import pandas as pd
from datetime import datetime
import plotly.graph_objects as go
import plotly.express as px
import random
import math

PATIENTS_FILE = "pacientes.json"

def _load_patient_registry():
    try:
        if not os.path.exists(PATIENTS_FILE):
            return {}
        with open(PATIENTS_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict):
            return data
        return {}
    except Exception:
        return {}

def _save_patient_registry(registry: dict) -> bool:
    try:
        with open(PATIENTS_FILE, "w", encoding="utf-8") as f:
            json.dump(registry, f, ensure_ascii=False, indent=2)
        return True
    except Exception:
        return False

def _approx_series_from_summary(session: dict, step: int = 6):
    try:
        dur = session.get("duracion_seg", session.get("time", session.get("duration", 0)))
        dur = int(dur or 0)
        if dur <= 0:
            return [], []
        total_count = session.get("contador", session.get("pulsaciones", session.get("count", session.get("c", 0))))
        total_count = int(total_count or 0)
        avg_apm = int(round((total_count * 60.0) / dur)) if dur > 0 else 0

        step = int(step or 6)
        if step <= 0:
            step = 6
        n = max(2, dur // step)

        # Curva suave: inicio y fin más bajos, pico al centro (aprox. movimiento real)
        # Escala base entre 60% y 110% del promedio
        serie_t = [i * step for i in range(n)]
        if avg_apm <= 0:
            return serie_t, [0] * n

        base_min = avg_apm * 0.6
        base_max = avg_apm * 1.1

        # Ruido determinístico leve por fecha para que no todas sean idénticas
        seed_src = str(session.get("fecha", "")) + str(session.get("usuario", ""))
        seed = sum(ord(c) for c in seed_src) % 997
        rng = random.Random(seed)

        serie_apm = []
        for i in range(n):
            x = i / (n - 1)
            shape = math.sin(math.pi * x)  # 0..1..0
            val = base_min + (base_max - base_min) * shape
            val += rng.randint(-2, 2)
            serie_apm.append(max(0, int(round(val))))

        return serie_t, serie_apm
    except Exception:
        return [], []

class HallSensorMonitor:
    def __init__(self):
        # Inicialización de variables en session_state
        if 'contador' not in st.session_state:
            st.session_state.contador = 0
        if 'rpm' not in st.session_state:
            st.session_state.rpm = 0
        if 'rpm_level' not in st.session_state:
            st.session_state.rpm_level = 'Ninguno'
        if 'tiempo_inactivo' not in st.session_state:
            st.session_state.tiempo_inactivo = 0
        if 'tiempo_ejercicio' not in st.session_state:
            st.session_state.tiempo_ejercicio = 0
        if 'serial_port' not in st.session_state:
            st.session_state.serial_port = None
        if 'is_connected' not in st.session_state:
            st.session_state.is_connected = False
        if 'ronda_finalizada' not in st.session_state:
            st.session_state.ronda_finalizada = False
        if 'data_dir' not in st.session_state:
            st.session_state.data_dir = "datos_pacientes"
            os.makedirs(st.session_state.data_dir, exist_ok=True)
        if 'serial_buffer' not in st.session_state:
            st.session_state.serial_buffer = ""
        if 'lcd_state' not in st.session_state:
            st.session_state.lcd_state = {
                "line1": "Esperando...",
                "line2": "",
                "user": "Ninguno",
                "mode": "login"  # login, exercise, paused, finished
            }
        if 'last_session_data' not in st.session_state:
            st.session_state.last_session_data = None
        if 'show_history' not in st.session_state:
            st.session_state.show_history = False
        if 'apm_history' not in st.session_state:
            st.session_state.apm_history = []  # historial corto para filtrar picos
        if 'rpm_filtered' not in st.session_state:
            st.session_state.rpm_filtered = 0
        if 'freeze_charts' not in st.session_state:
            st.session_state.freeze_charts = False  # congelar gráficos al finalizar
        if 'mock_loaded' not in st.session_state:
            st.session_state.mock_loaded = False

        if 'patient_registry' not in st.session_state:
            st.session_state.patient_registry = _load_patient_registry()
        if 'auth_ok' not in st.session_state:
            st.session_state.auth_ok = False
        if 'current_pin' not in st.session_state:
            st.session_state.current_pin = None
        if 'auth_error' not in st.session_state:
            st.session_state.auth_error = None

        if 'has_valid_data' not in st.session_state:
            st.session_state.has_valid_data = False

    def send_command(self, command: str) -> bool:
        try:
            if not command:
                return False
            if not (st.session_state.serial_port and st.session_state.serial_port.is_open):
                return False
            payload = (command.strip() + "\n").encode("utf-8")
            st.session_state.serial_port.write(payload)
            try:
                st.session_state.serial_port.flush()
            except Exception:
                pass
            return True
        except Exception:
            return False

    def scan_serial_ports(self):
        """Escanea los puertos seriales disponibles"""
        import serial.tools.list_ports
        ports = serial.tools.list_ports.comports()
        available_ports = [port.device for port in ports]
        return available_ports

    def connect_serial(self, port, baudrate=115200):
        """Conecta al puerto serial seleccionado"""
        try:
            # Cerrar conexión previa si existe
            if st.session_state.serial_port and st.session_state.serial_port.is_open:
                st.session_state.serial_port.close()
                
            # Pequeña pausa antes de reconectar
            time.sleep(0.5)
                
            # Conectar con timeout más largo
            st.session_state.serial_port = serial.Serial(port, baudrate=baudrate, timeout=1.0)
            
            # Esperar a que Arduino se reinicie
            time.sleep(2.0)
            
            # Vaciar buffer de entrada
            if st.session_state.serial_port.in_waiting:
                st.session_state.serial_port.reset_input_buffer()

            # Reset de estado para evitar picos iniciales en gráficas
            st.session_state.serial_buffer = ""
            st.session_state.has_valid_data = False
            st.session_state.contador = 0
            st.session_state.rpm = 0
            st.session_state.rpm_filtered = 0
            st.session_state.rpm_level = 'Ninguno'
            st.session_state.apm_history = []
            st.session_state.exercise_active = False
            st.session_state.freeze_charts = False
            st.session_state.chart_data = {
                'tiempo': [],
                'apm': [],
                'activaciones': []
            }
                
            st.session_state.is_connected = True
            return True
        except Exception as e:
            st.session_state.is_connected = False
            return False

    def read_sensor_data(self):
        """Lee datos del sensor desde el puerto serial - Compatible con Arduino optimizado"""
        if st.session_state.serial_port and st.session_state.serial_port.is_open:
            try:
                # Leer todos los datos disponibles
                if st.session_state.serial_port.in_waiting:
                    # Leer datos y acumular en el búfer
                    new_data = st.session_state.serial_port.read(st.session_state.serial_port.in_waiting).decode('utf-8', errors='replace')
                    st.session_state.serial_buffer += new_data
                    
                    # Procesar líneas completas en el búfer
                    while '\n' in st.session_state.serial_buffer:
                        line, st.session_state.serial_buffer = st.session_state.serial_buffer.split('\n', 1)
                        
                        # Procesar datos JSON del Arduino optimizado
                        try:
                            data = json.loads(line)

                            # Marcar como válido solo cuando llega un paquete con datos de sensor
                            if ('apm' in data) or ('hall_count' in data) or ('pulses_2s' in data) or ('pulses_1s' in data):
                                st.session_state.has_valid_data = True
                            
                            # Mapear campos del Arduino a variables de la interfaz
                            if 'hall_count' in data:
                                st.session_state.contador = data['hall_count']
                            
                            if 'apm' in data:
                                # Filtrado para respuesta rápida en bajadas (anti-picos con subida suave)
                                apm_val = int(data['apm'])
                                pulses_window = data.get('pulses_1s', data.get('pulses_2s', -1))
                                pulses_window = int(pulses_window) if pulses_window is not None else -1
                                exercise_active = bool(data.get('exercise_active', getattr(st.session_state, 'exercise_active', False)))

                                # Reglas simples: si no hay ejercicio o no hubo pulsos en 2s, APM = 0
                                if not exercise_active or pulses_window == 0:
                                    apm_val = 0

                                prev = int(st.session_state.get('rpm_filtered', 0) or 0)
                                if apm_val <= prev:
                                    # Bajada rápida: reflejar casi inmediato
                                    apm_filtered = apm_val
                                else:
                                    # Subida suave: evitar picos por ruido
                                    alpha_up = 0.35
                                    apm_filtered = int(round(prev + alpha_up * (apm_val - prev)))

                                st.session_state.rpm_filtered = apm_filtered
                                st.session_state.rpm = apm_filtered
                                
                                # Determinar nivel de intensidad basado en APM filtrado
                                if apm_filtered >= 120:
                                    st.session_state.rpm_level = "Alto"
                                elif apm_filtered >= 90:
                                    st.session_state.rpm_level = "Medio"
                                elif apm_filtered >= 60:
                                    st.session_state.rpm_level = "Lento"
                                else:
                                    st.session_state.rpm_level = "Ninguno"
                            
                            # Procesar estado del ejercicio
                            if 'exercise_active' in data:
                                st.session_state.exercise_active = data['exercise_active']

                            # Capturar usuario actual desde Arduino (PIN -> Nombre)
                            if 'login_success' in data:
                                pin = str(data['login_success']).strip()
                                st.session_state.current_pin = pin
                                registry = st.session_state.get('patient_registry') or _load_patient_registry()
                                st.session_state.patient_registry = registry
                                if pin in registry and isinstance(registry.get(pin), dict) and registry[pin].get('nombre'):
                                    nombre = str(registry[pin].get('nombre')).strip()
                                    st.session_state.current_user = nombre
                                    st.session_state.lcd_state['user'] = nombre
                                    st.session_state.auth_ok = True
                                    st.session_state.auth_error = None
                                else:
                                    st.session_state.current_user = None
                                    st.session_state.lcd_state['user'] = "Ninguno"
                                    st.session_state.auth_ok = False
                                    st.session_state.auth_error = f"PIN no registrado: {pin}"
                            if 'logout' in data:
                                st.session_state.current_user = None
                                st.session_state.lcd_state['user'] = "Ninguno"
                                st.session_state.current_pin = None
                                st.session_state.auth_ok = False
                                st.session_state.auth_error = None

                            # Al iniciar ejercicio: descongelar y limpiar buffers de gráficos
                            if 'exercise_started' in data and st.session_state.get('auth_ok', False):
                                st.session_state.freeze_charts = False
                                # Reiniciar series para nueva sesión
                                st.session_state.chart_data = {
                                    'tiempo': [],
                                    'apm': [],
                                    'activaciones': []
                                }
                                st.session_state.apm_history = []
                                st.session_state.rpm_filtered = 0

                            # Guardar sesión al terminar ejercicio
                            if 'exercise_finished' in data and st.session_state.get('auth_ok', False):
                                try:
                                    fin = data['exercise_finished']
                                    pin = str(fin.get('user') or st.session_state.get('current_pin') or "").strip()
                                    registry = st.session_state.get('patient_registry') or _load_patient_registry()
                                    st.session_state.patient_registry = registry
                                    if pin and pin in registry and isinstance(registry.get(pin), dict) and registry[pin].get('nombre'):
                                        user = str(registry[pin].get('nombre')).strip()
                                    else:
                                        user = st.session_state.get('current_user') or pin or 'desconocido'
                                    dur_seg = int(fin.get('time', 0))
                                    contador = int(fin.get('count', 0))
                                    rpm_val = int(st.session_state.get('rpm', 0))

                                    # Serie real para gráficas/comparación (si existe)
                                    serie_apm = []
                                    serie_t = []
                                    try:
                                        cd = st.session_state.get('chart_data') or {}
                                        apm_list = cd.get('apm') or []
                                        if isinstance(apm_list, list) and apm_list:
                                            serie_apm = [int(x) for x in apm_list]
                                            # el Arduino reporta cada ~2s en ejercicio -> eje en segundos
                                            serie_t = [i * 2 for i in range(len(serie_apm))]
                                    except Exception:
                                        serie_apm = []
                                        serie_t = []
                                    # Derivar rendimiento
                                    if rpm_val >= 120:
                                        rendimiento = "Alto"
                                    elif rpm_val >= 90:
                                        rendimiento = "Medio"
                                    elif rpm_val >= 60:
                                        rendimiento = "Lento"
                                    else:
                                        rendimiento = "Ninguno"

                                    # Calcular nivel más usado a partir del historial de APM de la sesión
                                    niveles_prioridad = ["Alto", "Medio", "Bajo", "Apagado"]
                                    nivel_mas_usado = None
                                    try:
                                        apms = []
                                        if 'chart_data' in st.session_state and st.session_state.chart_data.get('apm'):
                                            apms = list(st.session_state.chart_data['apm'])
                                        # Contar ocurrencias por nivel según umbrales 60/90/120
                                        counts = {"Apagado": 0, "Bajo": 0, "Medio": 0, "Alto": 0}
                                        for v in apms:
                                            if v >= 120:
                                                counts["Alto"] += 1
                                            elif v >= 90:
                                                counts["Medio"] += 1
                                            elif v >= 60:
                                                counts["Bajo"] += 1
                                            else:
                                                counts["Apagado"] += 1
                                        if any(counts.values()):
                                            # Elegir el nivel con mayor conteo; desempate por prioridad
                                            nivel_mas_usado = sorted(counts.items(), key=lambda x: (-x[1], niveles_prioridad.index(x[0])))[0][0]
                                    except Exception:
                                        nivel_mas_usado = None

                                    session_data = {
                                        "usuario": user,
                                        "pin": pin if pin else None,
                                        "fecha": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                                        "rpm": rpm_val,
                                        "contador": contador,
                                        "duracion_seg": dur_seg,
                                        "rendimiento": rendimiento,
                                        "nivel_mas_usado": nivel_mas_usado if nivel_mas_usado else None,
                                        "serie_t": serie_t,
                                        "serie_apm": serie_apm,
                                    }
                                    ok = self.save_session_data(session_data)
                                    if ok:
                                        st.session_state.last_session_data = session_data
                                        # Abrir historial del usuario actual
                                        st.session_state.show_history = True
                                        st.session_state.selected_patient = user
                                        # Congelar gráficos (mostrar último estado sin actualizar)
                                        st.session_state.freeze_charts = True
                                except Exception:
                                    pass
                                    
                        except json.JSONDecodeError:
                            # Si no es JSON, ignorar la línea
                            pass
                                
            except Exception as e:
                # Intentar reconectar automáticamente
                try:
                    if st.session_state.serial_port:
                        port = st.session_state.serial_port.port
                        self.connect_serial(port)
                except:
                    pass

    def disconnect_serial(self):
        """Desconecta el puerto serial de forma segura"""
        if st.session_state.serial_port and st.session_state.serial_port.is_open:
            st.session_state.serial_port.close()
        st.session_state.is_connected = False

    def save_session_data(self, session_data):
        """Guarda los datos de la sesión"""
        try:
            user = session_data["usuario"]
            
            # Crear directorio para el usuario
            user_dir = os.path.join(st.session_state.data_dir, user)
            os.makedirs(user_dir, exist_ok=True)
            
            # Nombre de archivo
            timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
            filename = os.path.join(user_dir, f"sesion_{timestamp}.json")
            
            # Guardar archivo
            with open(filename, "w") as file:
                json.dump(session_data, file)
                
            return True
        except:
            return False

    def create_mock_sessions(self, paciente_id="3"):
        try:
            os.makedirs(st.session_state.data_dir, exist_ok=True)
            user_dir = os.path.join(st.session_state.data_dir, paciente_id)
            os.makedirs(user_dir, exist_ok=True)

            base = [
                {"apm": 58, "dur": 180},
                {"apm": 72, "dur": 240},
                {"apm": 86, "dur": 300},
                {"apm": 98, "dur": 300},
                {"apm": 112, "dur": 360},
            ]
            now = time.time()
            for i, b in enumerate(base):
                t_step = 6
                n_points = max(1, b["dur"] // t_step)
                serie_t = [j * t_step for j in range(n_points)]
                random.seed(100 + i)
                # Fases: 30s Lento, 10s Medio, alternar Alto/Medio, finalizar Bajo y Apagado
                steps_30 = max(1, 30 // t_step)  # 5
                steps_10 = max(1, 10 // t_step)  # 2
                # Objetivos por nivel
                VAL_LENTO = 65
                VAL_MEDIO = 95
                VAL_ALTO = 125
                VAL_BAJO = 55
                VAL_APAG = 10
                targets = []
                # 30s lento
                targets += [VAL_LENTO] * min(steps_30, n_points - len(targets))
                # 10s medio
                if len(targets) < n_points:
                    targets += [VAL_MEDIO] * min(steps_10, n_points - len(targets))
                # Alternar alto/medio hasta que queden ~20s
                remaining = n_points - len(targets)
                end_block = steps_10 * 2  # 20s finales: bajo(10s) + apag(10s)
                k = 0
                while remaining > end_block and len(targets) < n_points:
                    block = [VAL_ALTO] * steps_10 + [VAL_MEDIO] * steps_10
                    take = min(len(block), n_points - len(targets) - end_block)
                    targets += block[:take]
                    remaining = n_points - len(targets)
                    k += 1
                # Cierre: bajo 10s, apagado 10s (si hay espacio)
                if len(targets) < n_points:
                    targets += [VAL_BAJO] * min(steps_10, n_points - len(targets))
                if len(targets) < n_points:
                    targets += [VAL_APAG] * min(steps_10, n_points - len(targets))
                # Si faltan puntos, completar con medio
                if len(targets) < n_points:
                    targets += [VAL_MEDIO] * (n_points - len(targets))

                # Suavizado con EMA + ruido bajo alrededor del target escalonado
                alpha = 0.25
                last = max(0, targets[0] + random.randint(-2, 2))
                serie_apm = []
                for j in range(n_points):
                    target = targets[j]
                    noise = random.randint(-2, 2)
                    last = last + alpha * (target - last) + noise
                    val = max(0, int(round(last)))
                    serie_apm.append(val)
                contador = int(sum(serie_apm) * (t_step / 60.0))
                if b["apm"] >= 120:
                    nivel = "Alto"
                elif b["apm"] >= 90:
                    nivel = "Medio"
                elif b["apm"] >= 60:
                    nivel = "Bajo"
                else:
                    nivel = "Apagado"
                fecha_dt = datetime.fromtimestamp(now - (len(base) - i) * 86400)
                session_data = {
                    "usuario": paciente_id,
                    "fecha": fecha_dt.strftime("%Y-%m-%d %H:%M:%S"),
                    "rpm": int(round(sum(serie_apm) / len(serie_apm))) if serie_apm else b["apm"],
                    "contador": contador,
                    "duracion_seg": b["dur"],
                    "rendimiento": nivel,
                    "nivel_mas_usado": nivel,
                    "serie_t": serie_t,
                    "serie_apm": serie_apm,
                }
                ts_name = fecha_dt.strftime("%Y-%m-%d_%H-%M-%S")
                filename = os.path.join(user_dir, f"sesion_{ts_name}.json")
                with open(filename, "w") as f:
                    json.dump(session_data, f)
            st.session_state.mock_loaded = True
            return True
        except Exception:
            return False
    
    def get_rpm_promedio(self):
        """Calcula el RPM promedio de la sesión actual"""
        if 'rpm_historial' in st.session_state and st.session_state.rpm_historial:
            return sum(st.session_state.rpm_historial) / len(st.session_state.rpm_historial)
        elif 'rpm' in st.session_state:
            return st.session_state.rpm
        else:
            return 0
            
    def initialize_rpm_history(self):
        """Inicializa o actualiza el historial de RPM"""
        if 'rpm_historial' not in st.session_state:
            st.session_state.rpm_historial = []
        
        # Si hay un valor de RPM actual, añadirlo al historial
        if 'rpm' in st.session_state and st.session_state.rpm > 0:
            st.session_state.rpm_historial.append(st.session_state.rpm)

    
    def get_all_patients_data(self):
        """Obtiene todos los datos de los pacientes"""
        patients_data = {}
        
        try:
            # Recorrer todos los directorios de pacientes
            for patient_id in os.listdir(st.session_state.data_dir):
                patient_dir = os.path.join(st.session_state.data_dir, patient_id)
                
                # Verificar que sea un directorio
                if not os.path.isdir(patient_dir):
                    continue
                
                # Obtener todas las sesiones del paciente
                sessions = []
                for session_file in os.listdir(patient_dir):
                    if not session_file.endswith('.json'):
                        continue
                        
                    try:
                        with open(os.path.join(patient_dir, session_file), 'r') as f:
                            session_data = json.load(f)
                            sessions.append(session_data)
                    except:
                        pass
                
                # Ordenar sesiones por fecha (más reciente primero)
                sessions.sort(key=lambda x: x.get('fecha', ''), reverse=True)
                
                # Guardar datos del paciente
                patients_data[patient_id] = sessions
                
            return patients_data
        except:
            return {}

def toggle_history():
    """Alterna la visibilidad del historial"""
    st.session_state.show_history = not st.session_state.show_history

def toggle_patients_panel():
    """Alterna la visibilidad del panel de gestión de pacientes"""
    st.session_state.show_patients = not st.session_state.show_patients

def main():
    # Configuración de la página
    st.set_page_config(
        page_title="Monitor de Pacientes - Médico",
        page_icon="🏥",
        layout="wide",
        initial_sidebar_state="collapsed"
    )
    
    # Estilo personalizado
    st.markdown("""
    <style>
    /* Estilos generales */
    .main {
        padding: 1rem 1rem;
    }
    
    /* LCD simulado */
    .lcd-display {
        background-color: #1E3F66;
        color: #89CFF0;
        font-family: 'Courier New', monospace;
        padding: 20px;
        border-radius: 10px;
        text-align: left;
        margin-bottom: 20px;
        font-size: 24px;
        letter-spacing: 1px;
        line-height: 1.5;
        box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1), inset 0 0 20px rgba(0, 0, 0, 0.2);
        border: 1px solid #0D2C54;
    }
    
    /* Sesión finalizada */
    .finished-session {
        background-color: #d4edda;
        color: #155724;
        padding: 20px;
        border-radius: 10px;
        margin: 20px 0;
        border-left: 5px solid #28a745;
        box-shadow: 0 2px 4px rgba(0, 0, 0, 0.05);
    }
    
    /* Contenedor principal */+
    .content-container {
        background-color: #f8f9fa;
        padding: 20px;
        border-radius: 10px;
        box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
        margin-bottom: 20px;
    }
    
    /* Botones personalizados */
    .stButton>button {
        border-radius: 8px;
        font-weight: 500;
        transition: all 0.2s ease;
    }
    
    /* Título principal */
    h1 {
        color: #0D2C54;
        padding-bottom: 10px;
        border-bottom: 2px solid #0D2C54;
        margin-bottom: 5px;
    }

    /* Subtítulo de autores */
    .authors {
        color: #555;
        font-size: 0.9rem;
        margin-bottom: 20px;
        font-style: italic;
    }
    
    /* Contenedor de presentación */
    .presentation-container {
        background-color: #ffffff;
        padding: 40px;
        border-radius: 10px;
        box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
        margin: 20px 0;
        border-top: 4px solid #0D2C54;
        max-width: 800px;
        margin-left: auto;
        margin-right: auto;
    }
    
    /* Panel de historial */
    .history-panel {
        background-color: #ffffff;
        border-radius: 10px;
        padding: 20px;
        box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
        margin-top: 20px;
        border-top: 4px solid #0D2C54;
    }
    
    /* Métricas */
    .metric-card {
        background-color: #0D2C54;
        color: white;
        padding: 15px;
        border-radius: 8px;
        box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
        text-align: center;
    }
    
    .metric-card h4 {
        color: #89CFF0;
        margin-bottom: 10px;
        font-weight: 600;
    }
    
    .metric-card h2 {
        color: white;
        font-size: 2.2rem;
        margin: 0;
    }
    
    /* Tablas */
    .dataframe {
        border-collapse: collapse;
        width: 100%;
        border-radius: 8px;
        overflow: hidden;
    }
    
    .dataframe th {
        background-color: #0D2C54;
        color: white;
        padding: 12px;
        text-align: left;
    }
    
    .dataframe td {
        padding: 10px;
        border-bottom: 1px solid #e0e0e0;
    }
    
    .dataframe tr:nth-child(even) {
        background-color: #f5f5f5;
    }
    </style>
    """, unsafe_allow_html=True)
    
    # Variables de estado para la navegación
    if 'page' not in st.session_state:
        st.session_state.page = 'presentation'
    if 'show_patients' not in st.session_state:
        st.session_state.show_patients = False
    
    # Inicializar monitor
    monitor = HallSensorMonitor()
    
    # Función para cambiar de página
    def change_page(page):
        st.session_state.page = page
        
    # Contenedor principal
    with st.container():
        # Mostrar encabezado solo en la página principal (no en la presentación)
        if st.session_state.page == 'main':
            # Encabezado principal con diseño mejorado
            st.markdown("<h1>🏥 Monitor de Pacientes en Hemodiálisis</h1>", unsafe_allow_html=True)
            # Añadir nombres de los autores
            st.markdown("<div class='authors'>Desarrollado por: Arnold Enrique Gutierrez Perez y Carlos Rodolfo Florez Villalvazo - Ing. Mecatrónica</div>", unsafe_allow_html=True)
            
            # Barra de navegación principal
            col1, col2, col3, col4 = st.columns([1, 1, 1, 1])
            
            # Botón para volver a la presentación
            with col3:
                if st.button("📋 Presentación", use_container_width=True, type="secondary"):
                    change_page('presentation')

            # Botón de pacientes (alta/eliminar)
            with col2:
                if st.button("👥 Pacientes", use_container_width=True, type="primary" if st.session_state.show_patients else "secondary"):
                    toggle_patients_panel()
                    
            # Botón de historial siempre visible
            with col4:
                if st.button("📊 Historial", use_container_width=True, type="primary" if st.session_state.show_history else "secondary"):
                    toggle_history()
    
        # Añadir opciones de conexión directamente en la página principal
        with st.container():
            st.markdown('<div class="content-container">', unsafe_allow_html=True)
            
            if not st.session_state.is_connected:
                # Opciones de conexión
                st.markdown("### 🔌 Conexión al Dispositivo")
                
                col1, col2, col3 = st.columns([1, 1, 1])
                with col1:
                    if st.button("🔍 Buscar Puertos", use_container_width=True):
                        ports = monitor.scan_serial_ports()
                        if ports:
                            st.session_state.available_ports = ports
                            st.success(f"Encontrados {len(ports)} puertos: {', '.join(ports)}")
                        else:
                            st.error("No se encontraron dispositivos")
                
                with col2:
                    port = None
                    if 'available_ports' in st.session_state and st.session_state.available_ports:
                        port = st.selectbox("Puerto:", st.session_state.available_ports)
                    else:
                        port = st.text_input("Puerto (ej: COM3):")
                
                with col3:
                    if st.button("🔌 Conectar", use_container_width=True):
                        if port:
                            success = monitor.connect_serial(port)
                            if success:
                                st.success(f"Conectado a {port}")
                            else:
                                st.error(f"Error al conectar a {port}")
                        else:
                            st.warning("Seleccione o ingrese un puerto")
            else:
                # Título cuando está conectado
                st.markdown("### 🟢 Dispositivo Conectado")
                
                # Opciones cuando está conectado
                if st.button("❌ Desconectar", type="secondary"):
                    monitor.disconnect_serial()
                    st.rerun()
                
            st.markdown('</div>', unsafe_allow_html=True)

        # Panel de gestión de pacientes (alta/eliminar)
        if st.session_state.show_patients:
            st.markdown('<div class="history-panel">', unsafe_allow_html=True)
            st.markdown("### � Gestión de Pacientes")

            st.markdown("#### ➕ Alta de Paciente")
            col_reg1, col_reg2, col_reg3 = st.columns([2, 1, 1])
            with col_reg1:
                nuevo_nombre = st.text_input("Nombre del Paciente", key="reg_nombre")
            with col_reg2:
                nuevo_pin_raw = st.text_input("PIN (2 dígitos)", max_chars=2, key="reg_pin")
            with col_reg3:
                if st.button("Guardar", key="reg_guardar"):
                    nombre = (nuevo_nombre or "").strip()
                    pin = (nuevo_pin_raw or "").strip()
                    if not nombre:
                        st.error("Ingrese un nombre válido")
                    elif len(pin) != 2 or (not pin.isdigit()):
                        st.error("El PIN debe ser numérico de 2 dígitos (00-99)")
                    else:
                        registry = st.session_state.get('patient_registry') or _load_patient_registry()
                        registry[str(pin)] = {"nombre": nombre}
                        if _save_patient_registry(registry):
                            st.session_state.patient_registry = registry
                            st.success(f"Paciente guardado: {nombre} (PIN {pin})")
                            st.rerun()
                        else:
                            st.error("No se pudo guardar el registro de pacientes")

            st.markdown("#### 🗑️ Eliminar Paciente")
            col_delp1, col_delp2, col_delp3 = st.columns([2, 1, 1])
            with col_delp1:
                pin_eliminar = st.text_input("PIN a eliminar (2 dígitos)", max_chars=2, key="del_pin")
            with col_delp2:
                confirmar = st.checkbox("Confirmar eliminación", key="del_confirm")
            with col_delp3:
                if st.button("Eliminar", key="del_btn"):
                    pin = (pin_eliminar or "").strip()
                    if len(pin) != 2 or (not pin.isdigit()):
                        st.error("El PIN debe ser numérico de 2 dígitos (00-99)")
                    elif not confirmar:
                        st.warning("Activa 'Confirmar eliminación' para continuar")
                    else:
                        registry = st.session_state.get('patient_registry') or _load_patient_registry()
                        if pin not in registry:
                            st.warning(f"El PIN {pin} no existe en el registro")
                        else:
                            try:
                                del registry[pin]
                            except Exception:
                                registry.pop(pin, None)
                            if _save_patient_registry(registry):
                                st.session_state.patient_registry = registry
                                st.success(f"PIN {pin} eliminado del registro (no se borraron sesiones)")
                                st.rerun()
                            else:
                                st.error("No se pudo guardar el registro tras eliminar")

            st.markdown('</div>', unsafe_allow_html=True)
    
        # Sección de historial (mostrar/ocultar)
        if st.session_state.show_history:
            st.markdown('<div class="history-panel">', unsafe_allow_html=True)
            # Botón para eliminar todos los datos anteriores
            col_del1, col_del2 = st.columns([3,1])
            with col_del2:
                if st.button("🗑️ Eliminar datos anteriores", type="secondary"):
                    try:
                        import shutil
                        data_dir = st.session_state.data_dir
                        # Borrar contenido del directorio de datos
                        for name in os.listdir(data_dir):
                            path = os.path.join(data_dir, name)
                            if os.path.isdir(path):
                                shutil.rmtree(path, ignore_errors=True)
                            else:
                                try:
                                    os.remove(path)
                                except:
                                    pass
                        st.success("Datos anteriores eliminados")
                        st.rerun()
                    except Exception as e:
                        st.error(f"No se pudo eliminar: {e}")
            st.markdown("### 📊 Historial de Pacientes")
            
            # Obtener datos de todos los pacientes
            patients_data = monitor.get_all_patients_data()
            # Auto-generar sesiones de prueba para paciente 3 si no existen
            try:
                if ("3" not in patients_data) or (not patients_data.get("3")):
                    if monitor.create_mock_sessions("3"):
                        patients_data = monitor.get_all_patients_data()
                        st.session_state.selected_patient = "3"
            except Exception:
                pass
            
            if patients_data:
                # Lista de pacientes
                patient_ids = list(patients_data.keys())

                registry = st.session_state.get('patient_registry') or _load_patient_registry()
                st.session_state.patient_registry = registry
                display_to_id = {}
                display_options = []
                for pid in patient_ids:
                    nombre = None
                    if isinstance(registry, dict) and pid in registry and isinstance(registry.get(pid), dict):
                        nombre = registry[pid].get('nombre')
                    label = (str(nombre).strip() if nombre else str(pid))
                    if label in display_to_id:
                        label = f"{label} ({pid})"
                    display_to_id[label] = pid
                    display_options.append(label)

                # Selección por defecto al paciente actual, si está disponible
                default_index = 0
                if 'selected_patient' in st.session_state and st.session_state.selected_patient in patient_ids:
                    sel = st.session_state.selected_patient
                    sel_nombre = None
                    if isinstance(registry, dict) and sel in registry and isinstance(registry.get(sel), dict):
                        sel_nombre = registry[sel].get('nombre')
                    sel_label = (str(sel_nombre).strip() if sel_nombre else str(sel))
                    if sel_label in display_to_id and display_to_id.get(sel_label) == sel:
                        default_index = display_options.index(sel_label)
                    else:
                        # si hay duplicados, caer al primero disponible
                        for i, lab in enumerate(display_options):
                            if display_to_id.get(lab) == sel:
                                default_index = i
                                break

                selected_label = st.selectbox("Seleccionar Paciente:", display_options, index=default_index)
                selected_patient = display_to_id.get(selected_label)
                
                if selected_patient and selected_patient in patients_data:
                    sessions = patients_data[selected_patient]
                    
                    if sessions:
                        # Convertir a DataFrame para mejor visualización
                        def to_mmss(total_seconds):
                            try:
                                total_seconds = int(total_seconds)
                                m = total_seconds // 60
                                s_ = total_seconds % 60
                                return f"{m:02d}:{s_:02d}"
                            except:
                                return ""

                        rows = []
                        for s in sessions:
                            rpm_val = s.get("rpm", 0)
                            # Derivar rendimiento según APM (60/90/120)
                            if rpm_val >= 120:
                                rendimiento = "Alto"
                            elif rpm_val >= 90:
                                rendimiento = "Medio"
                            elif rpm_val >= 60:
                                rendimiento = "Lento"
                            else:
                                rendimiento = "Ninguno"

                            # Intentar obtener duración en segundos de distintas claves comunes
                            dur_seg = s.get("time", s.get("duracion_seg", s.get("duration", 0)))
                            dur_mmss = s.get("duracion_mmss", to_mmss(dur_seg)) if dur_seg else ""

                            # Calcular/leer nivel más usado (Bajo/Medio/Alto/Apagado)
                            nivel_mas_usado = s.get("nivel_mas_usado")
                            if not nivel_mas_usado:
                                try:
                                    v = rpm_val
                                    if v >= 120:
                                        nivel_mas_usado = "Alto"
                                    elif v >= 90:
                                        nivel_mas_usado = "Medio"
                                    elif v >= 60:
                                        nivel_mas_usado = "Bajo"
                                    else:
                                        nivel_mas_usado = "Apagado"
                                except:
                                    nivel_mas_usado = "Apagado"

                            # Mostrar el nivel más usado dentro de 'Rendimiento'
                            rows.append({
                                "Fecha": s.get("fecha", ""),
                                "RPM": rpm_val,
                                "Pulsaciones": s.get("contador", s.get("pulsaciones", s.get("c", 0))),
                                "Duración": dur_mmss,
                                "Rendimiento": nivel_mas_usado,
                            })

                        df_sessions = pd.DataFrame(rows)
                        
                        # Estadísticas del paciente
                        st.markdown(f"#### Paciente: {selected_label}")
                        
                        if not df_sessions.empty:
                            col1, col2 = st.columns(2)
                            
                            with col1:
                                total_sessions = len(df_sessions)
                                st.markdown(f"""
                                <div class="metric-card">
                                    <h4>Total Sesiones</h4>
                                    <h2>{total_sessions}</h2>
                                </div>
                                """, unsafe_allow_html=True)
                            
                            with col2:
                                avg_rpm = round(df_sessions["RPM"].mean(), 1)
                                st.markdown(f"""
                                <div class="metric-card">
                                    <h4>Promedio RPM</h4>
                                    <h2>{avg_rpm}</h2>
                                </div>
                                """, unsafe_allow_html=True)
                            
                            # Si hay duraciones válidas, mostrar una métrica de duración total
                            if not df_sessions["Duración"].replace("", pd.NA).isna().all():
                                def parse_mmss(mmss):
                                    try:
                                        m, s_ = mmss.split(":")
                                        return int(m)*60 + int(s_)
                                    except:
                                        return 0
                                total_seconds = int(df_sessions["Duración"].apply(parse_mmss).sum())
                                total_m = total_seconds // 60
                                total_s = total_seconds % 60
                                st.markdown(f"""
                                <div class="metric-card" style="margin-top: 10px;">
                                    <h4>Tiempo Total</h4>
                                    <h2>{total_m:02d}:{total_s:02d}</h2>
                                </div>
                                """, unsafe_allow_html=True)
                                
                            # Tabla de sesiones
                            st.markdown("#### Detalle de Sesiones")
                            st.dataframe(df_sessions)

                            st.markdown("#### Gráfica por Sesión")
                            for idx, s in enumerate(sessions):
                                with st.expander(f"Sesión {s.get('fecha','')} - RPM {s.get('rpm',0)}"):
                                    serie_t = s.get("serie_t", [])
                                    serie_apm = s.get("serie_apm", [])
                                    if not serie_apm:
                                        try:
                                            dur = int(s.get("duracion_seg", s.get("duration", 0)) or 0)
                                        except:
                                            dur = 0
                                        if dur > 0:
                                            serie_t, serie_apm = _approx_series_from_summary(s, step=6)
                                    if serie_apm:
                                        fig_s = go.Figure()
                                        fig_s.add_trace(go.Scatter(x=serie_t if serie_t else list(range(len(serie_apm))), y=serie_apm, mode='lines+markers', line=dict(width=2)))
                                        fig_s.update_layout(height=250, margin=dict(l=0, r=0, t=10, b=0), xaxis_title="Tiempo (s)", yaxis_title="APM")
                                        st.plotly_chart(fig_s, use_container_width=True, key=f"session_chart_{selected_patient}_{idx}")
                                    else:
                                        st.info("Sin datos de serie para esta sesión")

                            st.markdown("#### Comparación de Sesiones")
                            opciones = [f"{i+1}. {s.get('fecha','')} (RPM {s.get('rpm',0)})" for i, s in enumerate(sessions)]
                            seleccion = st.multiselect("Seleccione sesiones a comparar", opciones, default=opciones[:3])
                            if seleccion:
                                fig_cmp = go.Figure()
                                colores = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd", "#8c564b"]
                                for sel in seleccion:
                                    try:
                                        idx = opciones.index(sel)
                                    except ValueError:
                                        continue
                                    s = sessions[idx]
                                    serie_t = s.get("serie_t", [])
                                    serie_apm = s.get("serie_apm", [])
                                    if not serie_apm:
                                        try:
                                            dur = int(s.get("duracion_seg", s.get("duration", 0)) or 0)
                                        except:
                                            dur = 0
                                        if dur > 0:
                                            serie_t, serie_apm = _approx_series_from_summary(s, step=6)
                                    color = colores[seleccion.index(sel) % len(colores)]
                                    fig_cmp.add_trace(go.Scatter(x=serie_t if serie_t else list(range(len(serie_apm))), y=serie_apm, mode='lines+markers', name=s.get('fecha',''), line=dict(width=3, color=color)))
                                fig_cmp.update_layout(height=350, margin=dict(l=0, r=0, t=20, b=0), xaxis_title="Tiempo (s)", yaxis_title="APM", legend_title_text="Sesiones")
                                st.plotly_chart(fig_cmp, use_container_width=True, key="comparison_chart")
                    else:
                        st.info(f"No hay sesiones registradas para el paciente {selected_patient}")
            else:
                st.info("No hay datos de pacientes disponibles")
                
            st.markdown('</div>', unsafe_allow_html=True)
        # Mostrar la página correspondiente según el estado
        if st.session_state.page == 'main':
            # Pantalla principal - Reflejo del LCD
            if st.session_state.is_connected:
                # Leer datos seriales
                monitor.read_sensor_data()
                if st.session_state.get('auth_error'):
                    st.error(st.session_state.auth_error)
                
                # Contenedor para el LCD
                st.markdown('<div class="content-container">', unsafe_allow_html=True)
                
                # Mostrar estado del ejercicio basado en datos del Arduino
                st.markdown("### 📺 Estado del Ejercicio")
                lcd_container = st.empty()
                
                # Mostrar estado basado en datos reales del Arduino
                exercise_active = getattr(st.session_state, 'exercise_active', False)
                
                if not st.session_state.get('auth_ok', False) and st.session_state.get('current_pin'):
                    estado_texto = "PIN no registrado"
                    detalle_texto = f"Registre el PIN {st.session_state.get('current_pin')} en 'Historial'"
                elif exercise_active and st.session_state.rpm > 0:
                    estado_texto = "Ejercicio Activo"
                    detalle_texto = f"Intensidad: {st.session_state.rpm_level} ({st.session_state.rpm} APM)"
                elif exercise_active:
                    estado_texto = "Ejercicio Iniciado"
                    detalle_texto = "Esperando actividad del sensor..."
                else:
                    estado_texto = "Esperando Actividad"
                    detalle_texto = "Inicie el ejercicio en el Arduino"
                    
                lcd_text = f"""<div class='lcd-display'>
                {estado_texto}<br>
                {detalle_texto}
                </div>"""
                
                lcd_container.markdown(lcd_text, unsafe_allow_html=True)

                # Si el PIN no está registrado, no mostrar métricas/gráficas ni guardar
                if (not st.session_state.get('auth_ok', False)) and st.session_state.get('current_pin'):
                    st.markdown('</div>', unsafe_allow_html=True)
                    if st.session_state.is_connected:
                        time.sleep(0.5)
                        st.rerun()
                    return

                # Controles de ejercicio desde la interfaz (sin reconectar)
                st.markdown("### 🎮 Control del Ejercicio")
                col_cmd1, col_cmd2, col_cmd3 = st.columns(3)
                with col_cmd1:
                    if st.button("▶️ Iniciar", key="cmd_start"):
                        if monitor.send_command("START"):
                            st.success("Comando START enviado")
                        else:
                            st.error("No se pudo enviar START")
                with col_cmd2:
                    if st.button("⏹️ Finalizar", key="cmd_stop"):
                        if monitor.send_command("STOP"):
                            st.success("Comando STOP enviado")
                        else:
                            st.error("No se pudo enviar STOP")
                with col_cmd3:
                    if st.button("🚪 Cerrar sesión", key="cmd_logout"):
                        if monitor.send_command("LOGOUT"):
                            st.success("Comando LOGOUT enviado")
                        else:
                            st.error("No se pudo enviar LOGOUT")

                # Botón rápido para ir al historial del usuario actual cuando hay ejercicio
                current_user = st.session_state.get('current_user')
                if exercise_active and current_user:
                    if st.button(f"📜 Historial de {current_user}"):
                        st.session_state.show_history = True
                        st.session_state.selected_patient = current_user
                        st.rerun()
                
                # Métricas en tiempo real del Arduino
                col1, col2, col3 = st.columns(3)
                
                with col1:
                    st.metric(
                        "Activaciones por Minuto", 
                        f"{st.session_state.rpm} APM",
                        delta=None
                    )
                
                with col2:
                    st.metric(
                        "Total de Activaciones", 
                        st.session_state.contador,
                        delta=None
                    )
                
                with col3:
                    st.metric(
                        "Nivel de Intensidad", 
                        st.session_state.rpm_level,
                        delta=None
                    )
                
                # Sección de gráficos en tiempo real
                frozen = st.session_state.get('freeze_charts', False)
                st.markdown("### 📊 Gráficos en Tiempo Real")
                
                # Inicializar datos históricos si no existen
                if 'chart_data' not in st.session_state:
                    st.session_state.chart_data = {
                        'tiempo': [],
                        'apm': [],
                        'activaciones': []
                    }
                
                # Agregar datos actuales al historial (máximo 50 puntos) solo si no está congelado
                if (
                    st.session_state.get('has_valid_data', False)
                    and (not st.session_state.get('freeze_charts', False))
                    and getattr(st.session_state, 'exercise_active', False)
                ):
                    current_time = datetime.now().strftime("%H:%M:%S")
                    st.session_state.chart_data['tiempo'].append(current_time)
                    st.session_state.chart_data['apm'].append(st.session_state.rpm)
                    st.session_state.chart_data['activaciones'].append(st.session_state.contador)
                
                # Mantener solo los últimos 50 puntos
                if len(st.session_state.chart_data['tiempo']) > 50:
                    for key in st.session_state.chart_data:
                        st.session_state.chart_data[key] = st.session_state.chart_data[key][-50:]
                
                # Crear gráficos con Plotly
                col_chart1, col_chart2 = st.columns(2)
                
                with col_chart1:
                    # Gráfico de APM en tiempo real
                    fig_apm = go.Figure()
                    fig_apm.add_trace(go.Scatter(
                        x=list(range(len(st.session_state.chart_data['apm']))),
                        y=st.session_state.chart_data['apm'],
                        mode='lines+markers',
                        name='APM',
                        line=dict(color='#1f77b4', width=3),
                        marker=dict(size=6)
                    ))
                    
                    fig_apm.update_layout(
                        title="Activaciones por Minuto (APM)",
                        xaxis_title="Tiempo",
                        yaxis_title="APM",
                        height=300,
                        showlegend=False,
                        margin=dict(l=0, r=0, t=30, b=0)
                    )
                    
                    st.plotly_chart(fig_apm, use_container_width=True, key="realtime_apm_chart")
                
                with col_chart2:
                    # Gráfico de activaciones totales
                    fig_total = go.Figure()
                    fig_total.add_trace(go.Scatter(
                        x=list(range(len(st.session_state.chart_data['activaciones']))),
                        y=st.session_state.chart_data['activaciones'],
                        mode='lines+markers',
                        name='Activaciones',
                        line=dict(color='#ff7f0e', width=3),
                        marker=dict(size=6)
                    ))
                    
                    fig_total.update_layout(
                        title="Total de Activaciones",
                        xaxis_title="Tiempo",
                        yaxis_title="Activaciones",
                        height=300,
                        showlegend=False,
                        margin=dict(l=0, r=0, t=30, b=0)
                    )
                    
                    st.plotly_chart(fig_total, use_container_width=True, key="realtime_total_chart")
                
                # Botón para nueva sesión
                if st.button("🔄 Nueva Sesión", use_container_width=False):
                    st.session_state.ronda_finalizada = False
                    st.session_state.last_session_data = None
                    # Limpiar datos de gráficos
                    st.session_state.chart_data = {
                        'tiempo': [],
                        'apm': [],
                        'activaciones': []
                    }
                    st.rerun()
            else:
                # Mensaje minimalista cuando no hay conexión
                st.info("Conecte el dispositivo Arduino para ver datos del paciente")
                
            # Auto-actualización cada 1 segundo mientras está conectado
            if st.session_state.is_connected:
                time.sleep(0.5)
                st.rerun()
            else:
                # Mensaje minimalista cuando no hay conexión
                st.info("Conecte el dispositivo Arduino para ver datos del paciente")
                
            # Auto-actualización cada 1 segundo mientras está conectado
            if st.session_state.is_connected:
                time.sleep(0.5)
                st.rerun()
        
        elif st.session_state.page == 'presentation':
            # Página de presentación con información de los autores
            st.markdown('<div class="content-container presentation-container">', unsafe_allow_html=True)
            
            # Logo y título
            st.markdown("""
            <div style="text-align: center; margin-bottom: 30px;">
                <h2>UNIVERSIDAD DE COLIMA</h2>
                <h3>FACULTAD DE INGENIERIA ELECTROMECÁNICA</h3>
                <h4>Campus El Naranjo</h4>
            </div>
            """, unsafe_allow_html=True)
            
            # Título del proyecto
            st.markdown("""
            <div style="text-align: center; margin: 40px 0;">
                <h2>SISTEMAS PARA EJERCICIO PARA<br>PACIENTES CON HEMODIÁLISIS</h2>
                <h3 style="margin-top: 30px;">TESIS</h3>
                <p style="margin-top: 20px;">Que para obtener el título en:</p>
                <h3>INGENIERO EN MECATRÓNICA</h3>
            </div>
            """, unsafe_allow_html=True)
            
            # Autores
            st.markdown("""
            <div style="text-align: center; margin: 40px 0;">
                <p>Presentan:</p>
                <h3>Arnold Enrique Gutiérrez Pérez</h3>
                <h3>Carlos Rodolfo Flores Villalvazo</h3>
                <p style="margin-top: 30px;">Asesor:</p>
                <h3>M.I Fidel Chávez Montejano</h3>
            </div>
            """, unsafe_allow_html=True)
            
            # Fecha
            st.markdown("""
            <div style="text-align: center; margin-top: 50px;">
                <p>Manzanillo, Col., México, 30 de mayo de 2025</p>
            </div>
            """, unsafe_allow_html=True)
            
            # Botón para iniciar la aplicación
            st.markdown("""
            <div style="text-align: center; margin-top: 50px;">
                <h3>Sistema de Monitoreo para Pacientes en Hemodiálisis</h3>
                <p>Presione el botón para iniciar el sistema de monitoreo</p>
            </div>
            """, unsafe_allow_html=True)

            
            # Botón grande y centrado para iniciar la aplicación
            col1, col2, col3 = st.columns([1, 2, 1])
            with col2:
                if st.button("🚀 INICIAR SISTEMA", use_container_width=True, type="primary"):
                    change_page('main')
            
            st.markdown('</div>', unsafe_allow_html=True)

if __name__ == "__main__":
    main()
