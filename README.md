# TP2 - Transferencia de Arquivos Peer-to-Peer

Esta pasta concentra a implementacao do Trabalho Pratico 2 em C, os scripts de teste e um rascunho de relatorio alinhado ao enunciado convertido do PDF.

## Estrutura

- [p2p_peer.c](p2p_peer.c): peer unico que atua como servidor e cliente ao mesmo tempo.
- [Makefile](Makefile): compilacao do executavel principal.
- [README.md](README.md): instrucoes de uso.
- [relatorio_tp2.md](relatorio_tp2.md): rascunho do relatorio do TP2.
- [testes/](testes/): scripts para gerar arquivos e executar cenarios.

## Visao geral da implementacao

O peer usa sockets TCP com `select()` para concorrencia e I/O nao bloqueante. O protocolo e simples:

- o seeder carrega o arquivo de origem com `--input`;
- ao iniciar, ele gera um arquivo `.meta` com nome, tamanho, tamanho do bloco, quantidade de blocos e SHA-256;
- os vizinhos sao informados por `--peer IP:PORT` e formam uma lista estaticamente definida;
- o leecher solicita blocos ate remontar o arquivo completo;
- quando um bloco chega, ele fica imediatamente disponivel para responder requisicoes de outros peers.

## Compilacao

```bash
make
```

Se o ambiente nao tiver `make`, compile diretamente:

```bash
gcc -O2 -Wall -Wextra -std=c11 -o p2p_peer p2p_peer.c
```

## Uso rapido

Seeder inicial:

```bash
./p2p_peer --listen 127.0.0.1:5000 --input arquivo.bin --peer 127.0.0.1:5001
```

Leecher:

```bash
./p2p_peer --listen 127.0.0.1:5001 --meta arquivo.bin.meta --output download.bin --peer 127.0.0.1:5000
```

O bloco padrao e de 1024 bytes. Se necessario, ajuste com `--block-size 4096`.

## Formato do metadado

O arquivo `.meta` e texto puro e contem:

- `filename`
- `filesize`
- `block_size`
- `block_count`
- `sha256`

## Testes e estudos de caso

Use os scripts em [testes/](testes/) para gerar os arquivos de entrada e reproduzir os cenarios do enunciado.

O arquivo [testes/README.md](testes/README.md) explica como criar os arquivos A, B e C, como rodar uma demo local com 2 ou 4 peers e como salvar logs para o relatorio.

## Observacoes

- O leecher pode iniciar com `--meta` ou esperar o metadado de um vizinho.
- O programa permanece ativo apos concluir o download para continuar servindo blocos.
- Use `--stop-on-complete` apenas em demonstracoes curtas.