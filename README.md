# Coco-Package-Manager
Coco-Package-Manager 

# Commands
coco sync               Actualizar lista de Repositorios
coco install <pkg>      Instalar (o actualizar si ya existe) un paquete
coco upgrade <pkg>      Actualizar un paquete, o todos si se omite
coco remove <pkg>       Eliminar un paquete instalado
coco search <term>      Buscar paquetes en el indice
coco list               Listar paquetes instalados
coco info <pkg>         Informacion detallada de un paquete
coco log <pkg>          Ver log de instalacion

# Ejemplos
coco sync
coco install fastfetch
coco upgrade            # actualiza todo lo instalado
coco upgrade neofetch   # actualiza solo ese paquete

# Instalar todo lo que coco necesita
debian - ubuntu - derivados - sudo apt install meson ninja-build gcc libcurl4-openssl-dev libarchive-dev libsqlite3-dev libjansson-dev

Fedora / RHEL / CentOS / Almalinux / openSUSE
sudo dnf install meson ninja-build gcc libcurl-devel libarchive-devel sqlite-devel jansson-devel
sudo zypper install meson ninja gcc libcurl-devel libarchive-devel sqlite3-devel jansson-devel

# Compilar una vez instaladas
dentro de la carpeta ejecuta
meson setup build
ninja -C build
sudo ninja -C build install

# Estructura del repositorio
repositorio
 | index.json    # Debe de estar en la raiz del repositorio
 | fastfetch     # la carpeta del paquete
   | fastfetch-1.0.0.tar.gz   # el paquete debe de tener adentro el binario hooks, etc. y debe de estar nombrado por version
   | fastfetch-1.0.0.tar.gz.sha256    # El paquete con extension .sha256
   | manifest.json    # El archivo que tiene los caracteres del cheksum sha256sum nombre del paquete, version, descripcion

# Notas Tecnicas
coco solo ha sido probado en Debian, Ubuntu, openSUSE y distros derivadas de ellos. asi que coco puede presentar anomalias en distribuciones como Arch linux y derivadas como slacware y otras que son roling release o sus paquetes suelen cambiar mucho.
coco todavia no esta hecho para resolver el conflicto de dependencias pero lo vamos a ir agregando poco a poco.
si tienes dudas de como o que archivos van en tu repositorio para usar coco deje los archivos para que puedas descargarlos y ponerlos COMO EJEMPLO en tu repositorio.
nos inspiramos de spm Simple-Package-Manager que esta bajo licensia MIT y tambien nos inspiramos de zypper.
coco no se apega a ningun sistema operativo asi que es universal SOLO en las distribuciones que se ha probado. fue creado en ubuntu 22.04 peto actualmente su desarollo se esta haciendo en openSUSE.
agregaremos libsolv pronto.

# Contactos, paginas y repositorios
https://coconutdynamics.com
apososgol1000@gmail.com
repo.coconutdynamics.com

# Otros proyectos
CoconutWAY OS - El escritorio reinventado
https://github.com/apososgol1000-png/coconutway-web
