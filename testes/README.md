# Testes do TP2

Esta pasta contem scripts para gerar arquivos de teste e rodar cenarios locais do peer-to-peer.

## 1. Gerar arquivos de teste

Crie os arquivos aleatorios do enunciado com:

```powershell
./gerar_arquivos_teste.ps1
```

O script gera uma pasta `dados/` com os seguintes arquivos:

- `file_a_10kb.bin`
- `file_a_20kb.bin`
- `file_b_1mb.bin`
- `file_b_5mb.bin`
- `file_c_10mb.bin`
- `file_c_20mb.bin`

Cada arquivo recebe um SHA-256 calculado ao final da geracao.

## 2. Rodar uma demonstracao local

Depois de compilar o peer, execute uma demo com 2 peers:

```powershell
./execucao_local.ps1 -Peers 2 -DataFile ..\dados\file_a_10kb.bin
```

Para 4 peers:

```powershell
./execucao_local.ps1 -Peers 4 -DataFile ..\dados\file_b_1mb.bin
```

O script cria uma topologia estatica e grava logs em `logs/`.

## 3. Variacoes do enunciado

Use o mesmo script mudando os parametros:

- `-BlockSize 1024` ou `-BlockSize 4096`
- `-Peers 2` ou `-Peers 4`

## 4. Resultado esperado

Ao final do download:

- o arquivo remontado deve ter o mesmo tamanho do original;
- o SHA-256 final deve coincidir com o SHA-256 do arquivo de origem;
- os logs devem mostrar recebimento e repasse dos blocos entre peers.