import time
import requests

# Configuración
# Reemplaza estos valores con los de tu canal de ThingSpeak.
# Recomendado: cargarlos desde variables de entorno en lugar de hardcodearlos.
ID_CANAL = 0000000
LLAVE_LECTURA = "YOUR_THINGSPEAK_READ_KEY"
LLAVE_ESCRITURA = "YOUR_THINGSPEAK_WRITE_KEY"

# Tiempos y reintentos
ESPERA_RATE_LIMIT = 16      # esperar antes de post para que ThingSpeak procese
ESPERA_ESP32 = 20           # esperar tras enviar para que el ESP32 lea el comando
REINTENTOS_ENVIO = 3        # número de reintentos al fallar el envío
ESPERA_REINTENTO = 3        # segundos entre reintentos


# 
# Utilidades
# 
def obtener_flotante(x):
    try:
        if x is None or x == "":
            return None
        return float(x)
    except Exception:
        return None


def obtener_entero(x):
    try:
        if x is None or x == "":
            return None
        return int(float(x))
    except Exception:
        return None


# 
# ThingSpeak I/O
# 
def leer_ultimo_feed():

    try:
        resp = requests.get(
            f"https://api.thingspeak.com/channels/{ID_CANAL}/feeds.json",
            params={"api_key": LLAVE_LECTURA, "results": 1},
            timeout=10,
        )
        resp.raise_for_status()
        data = resp.json()
        feeds = data.get("feeds", [])
        if not feeds:
            return {}
        return feeds[-1]
    except Exception as e:
        print("[ERROR] No se pudo leer ThingSpeak:", e)
        return {}


def mandar_comando(value):
    """Envía field7=value. Devuelve True si ThingSpeak aceptó el update."""
    # Respetar rate limit antes del primer intento
    time.sleep(ESPERA_RATE_LIMIT)
    attempt = 0
    while attempt < REINTENTOS_ENVIO:
        attempt += 1
        try:
            resp = requests.post(
                "https://api.thingspeak.com/update.json",
                params={"api_key": LLAVE_ESCRITURA, "field7": int(value)},
                timeout=10,
            )
            body = resp.text.strip()
            if resp.status_code == 200 and body != "0":
                # Esperar al ESP32 para que procese
                time.sleep(ESPERA_ESP32)
                return True
            else:
                print(f"Intento {attempt}/{REINTENTOS_ENVIO}: ThingSpeak respondió status={resp.status_code}, body={body}")
        except Exception as e:
            print(f"Intento {attempt}/{REINTENTOS_ENVIO}: error al enviar comando: {e}")

        if attempt < REINTENTOS_ENVIO:
            print(f"Reintentando en {ESPERA_REINTENTO}s...")
            time.sleep(ESPERA_REINTENTO)

    print("[ERROR] Todos los intentos fallaron. No se pudo enviar el comando a ThingSpeak.")
    return False


# 
# Parseo y presentación
# 
def extraer_estado(feed):
    """Extrae los campos relevantes y devuelve un dict normalizado."""
    return {
        "tiempo_ocupado_min": obtener_flotante(feed.get("field1")),  # field1 = tiempo total ocupado (min simulados)
        "temp_c": obtener_flotante(feed.get("field2")),
        "hum_porc": obtener_flotante(feed.get("field3")),
        "gate_open": obtener_entero(feed.get("field4")),
        "ocupado": obtener_entero(feed.get("field5")),
        "tarifa_30": obtener_flotante(feed.get("field6")),
        "tarifa_total": obtener_flotante(feed.get("field8")),
    }


def mostrar_estado(state) -> None:
    print("\n        ESTADO ACTUAL        ")

    t_occ = state.get("tiempo_ocupado_min")
    if t_occ is None:
        tiempo_str = "--"
    else:
        tiempo_str = f"{t_occ:.1f}"
    print("Tiempo ocupado: ", f"{tiempo_str} min")

    temp = state.get("temp_c")
    temp_str = "--" if temp is None else f"{temp} °C"
    hum = state.get("hum_porc")
    hum_str = "--" if hum is None else f"{hum} %"

    print("Temperatura:    ", temp_str)
    print("Humedad:        ", hum_str)

    ocupado = state.get("ocupado")
    ocupado_str = "Desconocido" if ocupado is None else ("Ocupado" if ocupado else "Libre")
    print("Ocupación:      ", ocupado_str)

    gate_open = state.get("gate_open")
    gate_str = "Desconocido" if gate_open is None else ("Abierta" if gate_open else "Cerrada")
    print("Estado barrera: ", gate_str)

    tarifa_30 = state.get("tarifa_30")
    print("Tarifa 30 min:  ", f"{tarifa_30}" if tarifa_30 is not None else "--")

    tt = state.get("tarifa_total")
    print("Tarifa total:   ", f"{tt:.2f}" if tt is not None else "--")
    print("                                \n")


# 
# FUNCIONES
# 
def refrescar():
    feed = leer_ultimo_feed()
    if not feed:
        print("No hay datos disponibles en el canal.\n")
        return
    estado = extraer_estado(feed)
    mostrar_estado(estado)


def entrada():
    """Intentar abrir la entrada (field7=1) si libre y barrera cerrada."""
    feed = leer_ultimo_feed()
    if not feed:
        print("No hay datos actuales. Refresca e intenta de nuevo.\n")
        return
    estado = extraer_estado(feed)
    mostrar_estado(estado)

    if estado.get("tarifa_total") not in (None, 0.0) and estado.get("tarifa_total") != 0.0:
        print(f"La tarifa total actual es {estado.get('tarifa_total'):.2f}. Solo se permite abrir con tarifa 0.\n")
        return

    if estado.get("ocupado") is None or estado.get("gate_open") is None:
        print("Estado incompleto. No se puede abrir por seguridad.\n")
        return

    if estado.get("ocupado") == 1:
        print("La plaza está ocupada. No se puede abrir la ENTRADA.\n")
        return

    if estado.get("gate_open") == 1:
        print("La barrera ya aparece abierta. No se envía la ENTRADA.\n")
        return

    print("Enviando comando ENTRADA (field7=1)...")
    ok = mandar_comando(1)
    print("Envío completado." if ok else "Fallo al enviar.")


def salida():
    """Enviar SALIDA (field7=2) tras confirmar pago."""
    feed = leer_ultimo_feed()
    if not feed:
        print("No hay datos actuales. Refresca e intenta de nuevo.\n")
        return
    estado = extraer_estado(feed)
    mostrar_estado(estado)

    if estado.get("ocupado") != 1:
        print("El lugar no está ocupado. No hay salida que procesar.\n")
        return

    # RESUMEN DE CUENTA cuando se cierra la cuenta para pagar
    print("========== RESUMEN DE CUENTA ==========")
    print(f"Tarifa actual por 30 min: {estado.get('tarifa_30')}")

    tt = estado.get("tarifa_total")
    if tt is None:
        print("Total a pagar: --")
    else:
        print(f"Total a pagar: {tt:.2f}")

    t_occ = estado.get("tiempo_ocupado_min")
    if t_occ is None:
        print("Tiempo total ocupado: --")
    else:
        print(f"Tiempo total ocupado: {t_occ:.1f} min")
    print("=======================================\n")
 
    input("Presiona ENTER cuando el cliente haya pagado para mandar SALIDA...")
    print("Enviando comando SALIDA (field7=2)...")
    ok = mandar_comando(2)
    print("Envío completado." if ok else "Fallo al enviar.")


# 
# Main loop
# 
def menu_principal():
    print(" Cliente Python: Estacionamiento Inteligente ")
    print(f"Canal: {ID_CANAL}")
    print("field7: comandos (1=entrada, 2=salida)")
    print("field4: estado de barrera (0=cerrada, 1=abierta)")
    print("field1: tiempo ocupado (min simulados)\n")

    while True:
        try:
            print("Menú:")
            print("  1) Refrescar datos")
            print("  2) Abrir ENTRADA (estacionar coche)")
            print("  3) Abrir SALIDA (pagar y salir)")
            print("  0) Salir")
            opcion = input("Elige opción: ").strip()

            if opcion == "0":
                print("Saliendo...")
                break
            elif opcion == "1":
                refrescar()
            elif opcion == "2":
                entrada()
            elif opcion == "3":
                salida()
            else:
                print("Opción no válida.\n")

        except KeyboardInterrupt:
            print("\nInterrumpido por el usuario. Saliendo...")
            break
        except Exception as e:
            print(f"[ERROR] {e}")
            print("Reintentando en 10 s...")
            time.sleep(10)


if __name__ == "__main__":
    menu_principal()
