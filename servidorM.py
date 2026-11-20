import socket
import threading
import json
import time


# =====================================================
# CONFIGURAÇÕES
# =====================================================

HOST = "0.0.0.0"

UDP_PORT = 12345
TCP_PORT = 12346

BUFFER_SIZE = 65536
SOCKET_BUFFER = 4 * 1024 * 1024

UDP_IDLE_TIMEOUT = 2.5
UDP_END_GRACE = 0.7


# =====================================================
# FORMATAÇÃO
# =====================================================

def separador():
    print("-" * 55, flush=True)


# =====================================================
# CRIAÇÃO DE UMA SESSÃO UDP
# =====================================================

def criar_sessao_udp(endereco):

    agora = time.monotonic()

    return {
        "endereco": endereco,

        "seqs": set(),

        "recebidos": 0,
        "bytes": 0,

        "duplicados": 0,
        "fora_ordem": 0,
        "invalidos": 0,

        "ultima_seq": None,

        "primeiro_rx": agora,
        "ultimo_rx": agora,

        "expected": None,
        "end_recebido": None
    }


# =====================================================
# RELATÓRIO UDP
# =====================================================

def relatorio_udp(sessao, motivo):

    separador()

    print("[UDP] RESULTADO DO TESTE", flush=True)

    print(
        "Origem: {}:{}".format(
            sessao["endereco"][0],
            sessao["endereco"][1]
        ),
        flush=True
    )

    recebidos = sessao["recebidos"]
    unicos = len(sessao["seqs"])

    duplicados = sessao["duplicados"]
    fora_ordem = sessao["fora_ordem"]

    tempo = (
        sessao["ultimo_rx"] -
        sessao["primeiro_rx"]
    )

    if tempo <= 0:
        tempo = 0.000001

    freq_rx = recebidos / tempo

    print(
        "Datagramas recebidos: {}".format(recebidos),
        flush=True
    )

    print(
        "Sequencias unicas recebidas: {}".format(unicos),
        flush=True
    )

    print(
        "Duplicados: {}".format(duplicados),
        flush=True
    )

    print(
        "Fora de ordem: {}".format(fora_ordem),
        flush=True
    )

    print(
        "Bytes recebidos: {}".format(sessao["bytes"]),
        flush=True
    )

    print(
        "Frequencia recebida: {:.2f} pacotes/s".format(freq_rx),
        flush=True
    )

    print(
        "Tempo entre primeiro e ultimo pacote: {:.3f} s".format(tempo),
        flush=True
    )

    expected = sessao["expected"]

    if expected is not None:

        sequencias_validas = {
            seq for seq in sessao["seqs"]
            if 0 <= seq < expected
        }

        recebidos_validos = len(sequencias_validas)

        perdidos = max(0, expected - recebidos_validos)

        extras = sum(
            1 for seq in sessao["seqs"]
            if seq < 0 or seq >= expected
        )

        if expected > 0:
            perda = (perdidos / expected) * 100
        else:
            perda = 0.0

        print()
        print("Quantidade esperada: {}".format(expected))
        print("Pacotes validos: {}".format(recebidos_validos))
        print("Pacotes perdidos: {}".format(perdidos))

        print(
            "Porcentagem de perda: {:.2f}%".format(perda),
            flush=True
        )

        if extras > 0:
            print("Sequencias fora do intervalo: {}".format(extras))

    else:

        # Sem marcador final, não sabemos quantos pacotes
        # foram perdidos depois da última sequência recebida.

        if sessao["seqs"]:

            esperado_estimado = max(sessao["seqs"]) + 1

            dentro_intervalo = {
                seq for seq in sessao["seqs"]
                if 0 <= seq < esperado_estimado
            }

            lacunas = max(
                0,
                esperado_estimado - len(dentro_intervalo)
            )

            print()
            print("AVISO: marcador final UDP nao recebido.")
            print(
                "Maior sequencia observada: {}".format(
                    max(sessao["seqs"])
                )
            )

            print(
                "Lacunas observadas ate a ultima sequencia: {}".format(
                    lacunas
                )
            )

            print(
                "Perdas finais nao podem ser calculadas com precisao."
            )

        else:

            print("Nenhuma sequencia valida recebida.")

    print(
        "Finalizacao: {}".format(motivo),
        flush=True
    )

    separador()


# =====================================================
# SERVIDOR UDP
# =====================================================

def servidor_udp(stop_event):

    sock = socket.socket(
        socket.AF_INET,
        socket.SOCK_DGRAM
    )

    try:

        sock.setsockopt(
            socket.SOL_SOCKET,
            socket.SO_RCVBUF,
            SOCKET_BUFFER
        )

        sock.bind((HOST, UDP_PORT))

        sock.settimeout(0.5)

        buffer_real = sock.getsockopt(
            socket.SOL_SOCKET,
            socket.SO_RCVBUF
        )

        print(
            "[UDP] Servidor iniciado na porta {}".format(UDP_PORT),
            flush=True
        )

        print(
            "[UDP] Buffer de recepcao: {} bytes".format(buffer_real),
            flush=True
        )

        sessao = None

        ultimo_status = time.monotonic()

        while not stop_event.is_set():

            agora = time.monotonic()

            # -------------------------------------------------
            # Finalização por timeout
            # -------------------------------------------------

            if sessao is not None:

                if (
                    sessao["end_recebido"] is not None
                    and agora - sessao["end_recebido"] >= UDP_END_GRACE
                ):

                    relatorio_udp(
                        sessao,
                        "Marcador final recebido"
                    )

                    sessao = None

                elif (
                    agora - sessao["ultimo_rx"] >= UDP_IDLE_TIMEOUT
                    and sessao["end_recebido"] is None
                ):

                    relatorio_udp(
                        sessao,
                        "Timeout de inatividade"
                    )

                    sessao = None

            # -------------------------------------------------
            # Recebimento
            # -------------------------------------------------

            try:

                dados, endereco = sock.recvfrom(BUFFER_SIZE)

            except socket.timeout:

                continue

            except OSError:

                if stop_event.is_set():
                    break

                raise

            agora = time.monotonic()

            try:

                mensagem = json.loads(dados.decode("utf-8"))

            except (UnicodeDecodeError, json.JSONDecodeError):

                continue

            tipo = mensagem.get("type")

            # -------------------------------------------------
            # Marcador final
            # -------------------------------------------------

            if tipo == "end":

                if sessao is not None:

                    try:

                        esperado = int(mensagem["expected"])

                        if esperado >= 0:

                            sessao["expected"] = esperado

                            if sessao["end_recebido"] is None:

                                sessao["end_recebido"] = agora

                    except (KeyError, ValueError, TypeError):

                        print("[UDP] Marcador final invalido.")

                continue

            # -------------------------------------------------
            # Mensagem de telemetria
            # -------------------------------------------------

            if tipo != "data":

                continue

            try:

                seq = int(mensagem["seq"])

            except (KeyError, ValueError, TypeError):

                continue

            # Se uma sessão anterior ficou sem marcador final,
            # finaliza antes de iniciar a nova.

            if (
                sessao is not None
                and agora - sessao["ultimo_rx"] >= UDP_IDLE_TIMEOUT
            ):

                relatorio_udp(
                    sessao,
                    "Timeout antes de nova sessao"
                )

                sessao = None

            # Cria sessão quando chega o primeiro pacote.

            if sessao is None:

                sessao = criar_sessao_udp(endereco)

                print(
                    "\n[UDP] Novo teste recebido de {}:{}".format(
                        endereco[0],
                        endereco[1]
                    ),
                    flush=True
                )

            # -------------------------------------------------
            # Estatísticas
            # -------------------------------------------------

            sessao["recebidos"] += 1

            sessao["bytes"] += len(dados)

            sessao["ultimo_rx"] = agora

            if seq in sessao["seqs"]:

                sessao["duplicados"] += 1

            else:

                if (
                    sessao["ultima_seq"] is not None
                    and seq < sessao["ultima_seq"]
                ):

                    sessao["fora_ordem"] += 1

                sessao["seqs"].add(seq)

                if sessao["ultima_seq"] is None:
                    sessao["ultima_seq"] = seq
                else:
                    sessao["ultima_seq"] = max(
                        sessao["ultima_seq"],
                        seq
                    )

            # -------------------------------------------------
            # Status periódico
            # -------------------------------------------------

            if agora - ultimo_status >= 1.0:

                tempo = (
                    sessao["ultimo_rx"] -
                    sessao["primeiro_rx"]
                )

                if tempo <= 0:
                    tempo = 0.000001

                frequencia = sessao["recebidos"] / tempo

                print(
                    "[UDP] Recebidos: {} | Unicos: {} | "
                    "Freq: {:.2f} pacotes/s".format(
                        sessao["recebidos"],
                        len(sessao["seqs"]),
                        frequencia
                    ),
                    flush=True
                )

                ultimo_status = agora

    except OSError as erro:

        print("[UDP] Erro: {}".format(erro), flush=True)

    finally:

        sock.close()


# =====================================================
# PROCESSAMENTO DE UMA LINHA TCP
# =====================================================

def processar_linha_tcp(linha, tamanho_wire, stats):

    if not linha:
        return

    try:

        mensagem = json.loads(linha.decode("utf-8"))

    except (UnicodeDecodeError, json.JSONDecodeError):

        stats["invalidos"] += 1
        return

    tipo = mensagem.get("type")

    # -------------------------------------------------
    # Mensagem de telemetria
    # -------------------------------------------------

    if tipo == "data":

        try:

            seq = int(mensagem["seq"])

        except (KeyError, ValueError, TypeError):

            stats["invalidos"] += 1
            return

        stats["mensagens"] += 1

        stats["bytes_dados"] += tamanho_wire

        if seq in stats["seqs"]:

            stats["duplicados"] += 1

        else:

            if (
                stats["ultima_seq"] is not None
                and seq < stats["ultima_seq"]
            ):

                stats["fora_ordem"] += 1

            stats["seqs"].add(seq)

            if stats["ultima_seq"] is None:
                stats["ultima_seq"] = seq
            else:
                stats["ultima_seq"] = max(
                    stats["ultima_seq"],
                    seq
                )

    # -------------------------------------------------
    # Marcador final
    # -------------------------------------------------

    elif tipo == "end":

        try:

            esperado = int(mensagem["expected"])

            if esperado >= 0:
                stats["expected"] = esperado

        except (KeyError, ValueError, TypeError):

            stats["invalidos"] += 1


# =====================================================
# RELATÓRIO TCP
# =====================================================

def relatorio_tcp(stats, endereco, duracao):

    separador()

    print("[TCP] RESULTADO DO TESTE", flush=True)

    print(
        "Origem: {}:{}".format(
            endereco[0],
            endereco[1]
        ),
        flush=True
    )

    mensagens = stats["mensagens"]

    unicas = len(stats["seqs"])

    bytes_dados = stats["bytes_dados"]

    print(
        "Mensagens recebidas: {}".format(mensagens),
        flush=True
    )

    print(
        "Sequencias unicas: {}".format(unicas),
        flush=True
    )

    print(
        "Mensagens duplicadas: {}".format(
            stats["duplicados"]
        ),
        flush=True
    )

    print(
        "Mensagens fora de ordem: {}".format(
            stats["fora_ordem"]
        ),
        flush=True
    )

    print(
        "Linhas invalidas: {}".format(
            stats["invalidos"]
        ),
        flush=True
    )

    print(
        "Bytes totais recebidos: {}".format(
            stats["bytes_totais"]
        ),
        flush=True
    )

    print(
        "Bytes de dados recebidos: {}".format(
            bytes_dados
        ),
        flush=True
    )

    if duracao > 0:

        mbps = (bytes_dados * 8) / duracao / 1000000

        mb_s = bytes_dados / duracao / 1000000

        frequencia = mensagens / duracao

        print()

        print(
            "Tempo da conexao: {:.3f} segundos".format(duracao),
            flush=True
        )

        print(
            "Frequencia recebida: {:.2f} mensagens/s".format(
                frequencia
            ),
            flush=True
        )

        print(
            "Vazao: {:.4f} Mbps".format(mbps),
            flush=True
        )

        print(
            "Vazao: {:.4f} MB/s".format(mb_s),
            flush=True
        )

    expected = stats["expected"]

    if expected is not None:

        seq_validas = {
            seq for seq in stats["seqs"]
            if 0 <= seq < expected
        }

        recebidos_validos = len(seq_validas)

        perdidos = max(
            0,
            expected - recebidos_validos
        )

        print()
        print("Quantidade esperada: {}".format(expected))
        print("Mensagens validas: {}".format(recebidos_validos))
        print("Mensagens ausentes: {}".format(perdidos))

    else:

        print()
        print("AVISO: marcador final TCP nao recebido.")

        if stats["seqs"]:

            esperado_estimado = max(stats["seqs"]) + 1

            print(
                "Maior sequencia observada: {}".format(
                    max(stats["seqs"])
                )
            )

            print(
                "Quantidade minima inferida: {}".format(
                    esperado_estimado
                )
            )

    separador()


# =====================================================
# ATENDIMENTO DE UMA CONEXÃO TCP
# =====================================================

def atender_cliente_tcp(conn, endereco, stop_event):

    stats = {

        "seqs": set(),

        "mensagens": 0,

        "bytes_totais": 0,
        "bytes_dados": 0,

        "duplicados": 0,
        "fora_ordem": 0,
        "invalidos": 0,

        "ultima_seq": None,

        "expected": None
    }

    buffer = bytearray()

    inicio = time.monotonic()

    print(
        "\n[TCP] Cliente conectado: {}:{}".format(
            endereco[0],
            endereco[1]
        ),
        flush=True
    )

    conn.settimeout(1.0)

    try:

        conn.setsockopt(
            socket.SOL_SOCKET,
            socket.SO_RCVBUF,
            SOCKET_BUFFER
        )

        while not stop_event.is_set():

            try:

                dados = conn.recv(BUFFER_SIZE)

            except socket.timeout:

                continue

            except ConnectionResetError:

                print("[TCP] Conexao resetada pelo cliente.")
                break

            if not dados:
                break

            stats["bytes_totais"] += len(dados)

            buffer.extend(dados)

            # TCP é um fluxo de bytes.
            # Cada mensagem termina com '\n'.

            while True:

                posicao = buffer.find(b"\n")

                if posicao < 0:
                    break

                linha = bytes(buffer[:posicao])

                del buffer[:posicao + 1]

                processar_linha_tcp(
                    linha,
                    len(linha) + 1,
                    stats
                )

        # Processa eventual conteúdo restante sem '\n'.

        if buffer:

            processar_linha_tcp(
                bytes(buffer),
                len(buffer),
                stats
            )

    except OSError as erro:

        print("[TCP] Erro: {}".format(erro), flush=True)

    finally:

        duracao = time.monotonic() - inicio

        conn.close()

        relatorio_tcp(
            stats,
            endereco,
            duracao
        )


# =====================================================
# SERVIDOR TCP
# =====================================================

def servidor_tcp(stop_event):

    servidor = socket.socket(
        socket.AF_INET,
        socket.SOCK_STREAM
    )

    try:

        servidor.setsockopt(
            socket.SOL_SOCKET,
            socket.SO_REUSEADDR,
            1
        )

        servidor.bind((HOST, TCP_PORT))

        servidor.listen(5)

        servidor.settimeout(0.5)

        print(
            "[TCP] Servidor iniciado na porta {}".format(TCP_PORT),
            flush=True
        )

        while not stop_event.is_set():

            try:

                conn, endereco = servidor.accept()

            except socket.timeout:

                continue

            except OSError:

                if stop_event.is_set():
                    break

                raise

            thread = threading.Thread(
                target=atender_cliente_tcp,
                args=(conn, endereco, stop_event),
                daemon=True
            )

            thread.start()

    except OSError as erro:

        print("[TCP] Erro: {}".format(erro), flush=True)

    finally:

        servidor.close()


# =====================================================
# FUNÇÃO PRINCIPAL
# =====================================================

def main():

    stop_event = threading.Event()

    thread_udp = threading.Thread(
        target=servidor_udp,
        args=(stop_event,),
        daemon=True
    )

    thread_tcp = threading.Thread(
        target=servidor_tcp,
        args=(stop_event,),
        daemon=True
    )

    thread_udp.start()
    thread_tcp.start()

    print("\n====================================")
    print("       SERVIDOR DE TESTE ESP32")
    print("====================================")

    print("UDP: porta {}".format(UDP_PORT))
    print("TCP: porta {}".format(TCP_PORT))

    print("\nAguardando testes do ESP32...")
    print("Pressione CTRL+C para encerrar.")
    print("====================================\n")

    try:

        while True:
            time.sleep(1)

    except KeyboardInterrupt:

        print("\nEncerrando servidor...")

    finally:

        stop_event.set()

        thread_udp.join(timeout=2)
        thread_tcp.join(timeout=2)

        print("Servidor encerrado.")


if __name__ == "__main__":
    main()