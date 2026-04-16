# ping.win 
<img width="60" height="36" alt="image" src="https://github.com/user-attachments/assets/b2612f65-f80d-4645-a79e-906a123f9565" />

Um monitor de latência (ping) leve e robusto para Windows, que vive na system tray.

Totalmente desenvolvido sob uma arquitetura _lock-free_ de alto nível em C++, não consome absolutamente nenhum ciclo desnecessário de CPU, RAM ou GDI do sistema operacional.

## Arquitetura Técnica

- **Gestão de estado**: Toda a troca de dados entre threads ocorre via ponteiros inteligentes atômicos nativos do padrão C++. Zero race conditions, zero deadlocks de UI. 
- **Zero-Waste GDI Caching**: Ícones da bandeja são gerados sob demanda e guardados em um cache local otimizado. Recriações repetidas do `HICON` não existem, abolindo os recarregamentos redundantes de _Windows API_ e prevenindo *flickering* (piscadas).
- **Sem _Message Floods_ (Timer-Pull UI)**: Completamente livre do antipadrão `PostMessage` que engargala a fila de janelas. Nossa UI rastreia sua conexão "puxando" (`WM_TIMER`) a memória imutável apenas nos momentos calculados.
- **Resolução DNS assíncrona robusta**: Processos como "aplicar destino `google.com`" rodam perfeitamente assíncronos. Se o DNS estiver lento, sua bandeja notificará.
- **Zero Redraws invisíveis**: A interface usa sobrecarga de construtores (`operator==`) no estado de latência. Se o processador calcular que a tela a ser mostrada já é a atual, a injeção nativa na Taskbar é bloqueada proativamente para economizar energia.

## Como usar

1. Baixe o executável portatil `ping.win.exe`. O arquivo possuí dependências e componentes já linkados dinamicamente (~280kb).
2. Execute, e na barra de ferramentas inferior você verá a sigla do seu ping (`Ping: XX`).
    - **Cores Adaptativas**: Vai ficar **Verde** em baixo ping, amarelo-mesclado sob moderado e **Vermelho** durante latências agudas ou `X` sob perda de pacote.
3. Clique com seu **botão direito** sobre o pequeno programa. Acesse **Change Host**.
4. Escreva qual destino testar. Funciona com DNS e com IPs absolutos (`google.com`, `192.168.0.1`, `1.1.1.1`).
5. Caso necessite fechá-lo de forma imediata (e liberar com segurança a memória Cache criada), simplesmente clique no menu contexto **Exit**.

## Como Compilar (Para Desenvolvedores)

O software exige rigor de compiladores de nível C++. Recomendamos enfaticamente o **MinGW-w64** sem adição de wrappers ou geradores CMake para evitar Overhead no Binário Final.

```powershell
# Compilar recursos e inserir o icone
windres resource.rc -o resource.o
# Compilação  do kernel com vínculo
g++ main.cpp resource.o -o ping.win.exe -lws2_32 -liphlpapi -lgdi32 -mwindows -static-libgcc -static-libstdc++ -static -s
# remover o lixo
del resource.o
```

## Licença

Este projeto é de código aberto e está sob a licença [MIT](LICENSE).
