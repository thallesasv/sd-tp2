# Relatorio TP2 - Transferencia de Arquivos Peer-to-Peer

## 1. Objetivo

Implementar um sistema elementar de transferencia de arquivos no modelo Peer-to-Peer, no qual cada no atua simultaneamente como cliente e servidor, solicitando e compartilhando blocos de arquivo em paralelo.

## 2. Decisoes de projeto

### 2.1 Linguagem e arquitetura

A solucao foi escrita em C para manter acesso direto a sockets, arquivos e chamadas de sistema. Cada instancia do programa representa um peer unico, capaz de aceitar conexoes de entrada e iniciar conexoes de saida ao mesmo tempo.

### 2.2 Concorrencia e comunicacao

Foi utilizado `select()` com sockets TCP nao bloqueantes. Essa escolha evita a necessidade de threads para a parte de rede e permite que o mesmo processo:

- aceite novas conexoes;
- envie requisicoes de blocos;
- receba blocos de outros peers;
- sirva blocos que ja possui.

### 2.3 Fragmentacao e remontagem

O arquivo original e dividido em blocos de tamanho fixo. O tamanho padrao adotado foi 1024 bytes, com suporte a alteracao por parametro. Os blocos sao gravados diretamente no arquivo de saida na posicao correta, o que simplifica a remontagem e permite que um bloco recebido ja possa ser compartilhado com outros peers.

### 2.4 Metadados e integridade

O seeder gera um arquivo `.meta` com:

- nome do arquivo;
- tamanho total;
- tamanho do bloco;
- quantidade de blocos;
- hash SHA-256.

Esse arquivo e usado para coordernar a recuperacao do arquivo e para validar a integridade do resultado final.

## 3. Protocolo de mensagens

O protocolo usa um cabecalho binario simples com tipo da mensagem, indice do bloco e tamanho do payload. As mensagens principais sao:

- `META`: envia os metadados do arquivo;
- `BLOCK_REQUEST`: solicita um bloco especifico;
- `BLOCK_DATA`: entrega os bytes de um bloco;
- `DONE`: reservado para extensao futura;
- `HELLO`: reservado para extensao futura.

## 4. Testes previstos

Os testes foram organizados conforme a tabela do enunciado convertido. O conjunto principal considera:

- quantidade de peers: 2 e 4;
- tamanho do bloco: 1024 e 4096 bytes;
- arquivos A, B e C com tamanhos progressivos;
- topologia estatica de vizinhos.

### 4.1 Observacao sobre o arquivo C

No texto convertido, a linha do arquivo grande aparece com uma provavel incoerencia de OCR. A interpretacao adotada neste repositorio foi tratar a variacao como 20 MB, pois isso preserva a progressao natural dos casos de teste.

## 5. Como executar os testes

1. Gerar os arquivos de teste com [testes/gerar_arquivos_teste.ps1](testes/gerar_arquivos_teste.ps1).
2. Compilar o peer com `make` ou com `gcc` diretamente.
3. Executar o seeder e os leechers com os parametros descritos em [testes/README.md](testes/README.md).
4. Registrar os logs de cada peer e comparar o SHA-256 do arquivo final com o original.

## 6. Resultados a preencher

Esta secao deve ser completada apos a execucao local dos cenarios:

- tempo medio por combinacao de peers;
- tempo medio por tamanho de bloco;
- logs de recebimento de blocos;
- validacao do hash SHA-256;
- conclusoes sobre escalabilidade e fragmentacao.

## 7. Conclusao

A solucao proposta atende ao modelo P2P solicitado no enunciado, com transferencia por blocos, metadados locais, verificacao de integridade e possibilidade de servir blocos imediatamente apos o recebimento.