  DUNGEON OF DOOM  —  OpenGL + GLFW Edition         
                   Linguagem C                           
 
   COMPILAR:
   ─────────────────────────────────────────────────────────────
   Requer miniaudio.h (header-only, sem custo):
     wget https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h
     
 
 
   Windows (MinGW):
     gcc dungeon_gl.c -o dungeon.exe -lglfw3 -lopengl32 -lgdi32 -lm -lwinmm -lole32
     -I"C:/glfw/include" -L"C:/glfw/lib"
     (baixar GLFW em https://www.glfw.org/download.html)
 
   CONTROLES:
   ─────────────────────────────────────────────────────────────
   WASD / Setas  → Mover / Atacar adjacente
   ESPAÇO        → Atacar inimigo mais próximo
   1-5           → Usar item do inventário
   ENTER         → Confirmar (menu)
   R             → Reiniciar (após morte/vitória)
   ESC           → Sair
