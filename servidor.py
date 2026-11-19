
import socket
import struct
import threading
import time

# ==========================================
# CONFIGURACOES
# ==========================================

HOST = "0.0.0.0"

UDP_PORT = 12345
TCP_PORT = 12346

PACKET_SIZE = 64

# ==========================================
# SERVIDOR UDP
# ==========================================

def udp_server():

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    # Aumenta o buffer de recepcao
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)

    sock.bind((HOST, UDP_PORT))

    print(f"[UDP] Servidor iniciado na porta {UDP_PORT}")

    received = set()

    duplicates = 0
    out_of_order = 0
    total_bytes = 0

    last_seq = -1

    start = None
    last_report = None

    expected = None

    while True:

        data, addr = sock.recvfrom(2048)

        # ----------------------------------
        # MARCADOR DE FINALIZACAO
        # ----------------------------------

        if len(data) == 8 and data[:4] == b"END!":

            expected = struct.unpack("<I", data[4:8])[0]

            break

        # ----------------------------------
        # VALIDACAO DO PACOTE
        # ----------------------------------

        if len(data) != PACKET_SIZE:
            continue

        seq, timestamp = struct.unpack("<II", data[:8])

        if start is None:

            start = time.perf_counter()
            last_report = start

        total_bytes += len(data)

        # ----------------------------------
        # IDENTIFICACAO DE DUPLICADOS
        # ----------------------------------

        if seq in received:

            duplicates += 1

        else:

            received.add(seq)

        # ----------------------------------
        # IDENTIFICACAO FORA DE ORDEM
        # ----------------------------------

        if seq < last_seq:

            out_of_order += 1

        last_seq = max(last_seq, seq)

        # ----------------------------------
        # RELATORIO A CADA SEGUNDO
        # ----------------------------------

        now = time.perf_counter()

        if now - last_report >= 1:

            elapsed = now - start

            rate = len(received) / elapsed

            print(
                f"[UDP] Recebidos: {len(received)} | "
                f"Taxa: {rate:.2f} pacotes/s"
            )

            last_report = now

    sock.close()

    # ======================================
    # CALCULO DOS RESULTADOS
    # ======================================

    unique = len(received)

    expected = expected if expected is not None else 0

    lost = max(0, expected - unique)

    loss = (
        lost / expected * 100
        if expected > 0 else 0
    )

    elapsed = (
        time.perf_counter() - start
        if start is not None else 0
    )

    print("\n========== RESULTADO UDP ==========")

    print(f"Pacotes esperados:        {expected}")
    print(f"Pacotes unicos recebidos: {unique}")
    print(f"Pacotes perdidos:         {lost}")
    print(f"Pacotes duplicados:       {duplicates}")
    print(f"Pacotes fora de ordem:    {out_of_order}")

    print(f"Perda:                    {loss:.4f}%")

    print(f"Dados recebidos:          {total_bytes / 1024:.2f} KB")

    if elapsed > 0:

        print(f"Tempo de recepcao:        {elapsed:.3f} s")

        print(
            f"Taxa media:               {unique / elapsed:.2f} pacotes/s"
        )

    print("===================================\n")


# ==========================================
# SERVIDOR TCP
# ==========================================

def tcp_server():

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    server.bind((HOST, TCP_PORT))

    server.listen(5)

    print(f"[TCP] Servidor iniciado na porta {TCP_PORT}")

    while True:

        conn, addr = server.accept()

        print(f"\n[TCP] Conexao recebida de {addr}")

        conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)

        total_bytes = 0

        buffer = bytearray()

        complete_messages = 0

        start = time.perf_counter()

        last_report = start

        last_bytes = 0

        try:

            while True:

                data = conn.recv(65536)

                if not data:
                    break

                total_bytes += len(data)

                buffer.extend(data)

                # Cada mensagem possui 64 bytes
                complete_messages += len(buffer) // PACKET_SIZE

                # Mantem somente bytes incompletos
                remainder = len(buffer) % PACKET_SIZE

                if remainder:
                    buffer[:] = buffer[-remainder:]
                else:
                    buffer.clear()

                # ----------------------------------
                # RELATORIO A CADA SEGUNDO
                # ----------------------------------

                now = time.perf_counter()

                if now - last_report >= 1:

                    interval = now - last_report

                    delta = total_bytes - last_bytes

                    mbps = delta * 8 / interval / 1_000_000

                    print(
                        f"[TCP] {mbps:.3f} Mbps | "
                        f"{mbps / 8:.3f} MB/s | "
                        f"{complete_messages} mensagens"
                    )

                    last_report = now

                    last_bytes = total_bytes

        except ConnectionError:

            print("[TCP] Conexao interrompida!")

        finally:

            elapsed = time.perf_counter() - start

            mbps = (
                total_bytes * 8 / elapsed / 1_000_000
                if elapsed > 0 else 0
            )

            print("\n========== RESULTADO TCP ==========")

            print(f"Bytes recebidos:        {total_bytes}")

            print(f"Mensagens completas:    {complete_messages}")

            print(f"Bytes restantes:        {len(buffer)}")

            print(f"Tempo:                  {elapsed:.3f} s")

            print(f"Velocidade:             {mbps:.3f} Mbps")

            print(f"Velocidade:             {mbps / 8:.3f} MB/s")

            print("===================================\n")

            conn.close()


# ==========================================
# PROGRAMA PRINCIPAL
# ==========================================

if __name__ == "__main__":

    print("====================================")
    print("      SERVIDOR DE TESTE ESP32")
    print("====================================")

    threading.Thread(
        target=udp_server,
        daemon=True
    ).start()

    tcp_server()
