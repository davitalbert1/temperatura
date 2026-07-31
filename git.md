# Git Cheatsheet - Temperatura

## Setup Inicial do Repositório

### Inicializar Git

```bash
git init
```

### Adicionar repositório remoto

```bash
git remote add origin https://github.com/davitalbert1/temperatura.git
```

```bash
git remote set-url origin https://github.com/davitalbert1/temperatura.git
```

### Verificar remoto

```bash
git remote -v
```

### Definir branch principal

```bash
git branch -M main
```

---

# Configuração SSH para GitHub

## Gerar chave SSH

```bash
ssh-keygen -t ed25519 -C "davitalbert@gmail.com"
```

## Exibir chave pública

```bash
cat /c/Users/P0406/.ssh/id_ed25519.pub
```

Copiar a chave gerada e adicionar no GitHub em:

https://github.com/settings/keys

## Testar autenticação SSH

```bash
ssh -T git@github.com
```

Resposta esperada:

```text
Hi davitalbert1! You've successfully authenticated, but GitHub does not provide shell access.
```

## Alterar repositório para SSH

```bash
git remote set-url origin https://github.com/davitalbert1/temperatura.git
```

```bash
git remote set-url origin git@github.com:davitalbert1/temperatura.git
```

## Confirmar alteração

```bash
git remote -v
```

Resultado esperado:

```text
origin  git@github.com:davitalbert1/temperatura.git (fetch)
origin  git@github.com:davitalbert1/temperatura.git (push)
```

---

# Primeiro Push

```bash
git push -u origin main
```

Caso exista conflito de histórico:

```bash
git push -u origin main --force
```

---

# Fluxo de Commit

## Verificar alterações

```bash
git status
```

## Adicionar arquivos

```bash
git add .
```

## Criar commit

```bash
git commit -m "mensagem do commit"
```

## Enviar para GitHub

```bash
git push
```

## Forçar atualização do remoto (usar com cuidado)

```bash
git push -u origin main --force
```

---

# Comandos Úteis

## Baixar alterações do GitHub

```bash
git pull
```

## Buscar alterações sem aplicar

```bash
git fetch
```

## Ver histórico resumido

```bash
git log --oneline
```

## Ver histórico gráfico

```bash
git log --oneline --graph --decorate --all
```

## Ver branch atual

```bash
git branch
```

## Ver configuração remota

```bash
git remote -v
```

## Ver versão do Git

```bash
git --version
```
