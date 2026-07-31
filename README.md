# 🥥 Coco-Package-Manager

![License](https://img.shields.io/badge/License-MIT-green.svg)
![Language](https://img.shields.io/badge/Language-C-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Linux-orange.svg)
![Status](https://img.shields.io/badge/Status-Active-success.svg)

Un gestor de paquetes moderno y ligero diseñado específicamente para distribuiciones basadas en Debian y derivados. Coco combina la filosofía de simplicidad con herramientas poderosas para gestionar dependencias en tu sistema.

**Parte del ecosistema [Coconut Dynamics](https://coconutdynamics.com)**

---

## 📋 Tabla de Contenidos

- [Características](#características)
- [Requisitos Previos](#requisitos-previos)
- [Instalación](#instalación)
- [Uso Rápido](#uso-rápido)
- [Comandos Disponibles](#comandos-disponibles)
- [Estructura del Repositorio](#estructura-del-repositorio)
- [Configuración Avanzada](#configuración-avanzada)
- [Notas Técnicas](#notas-técnicas)
- [Contribuir](#contribuir)
- [Licencia](#licencia)
- [Contacto](#contacto)

---

## ✨ Características

- ✅ Gestor de paquetes ligero y eficiente
- ✅ Compatible con Debian, Ubuntu y derivados
- ✅ Instalación, actualización y eliminación de paquetes
- ✅ Sistema de búsqueda integrado
- ✅ Verificación de integridad (SHA256)
- ✅ Registro detallado de instalaciones
- ✅ Código abierto bajo licencia MIT
- ✅ Instalado en una sola línea

---

## 🔧 Requisitos Previos

### Para Debian/Ubuntu y derivados:
```bash
sudo apt install meson ninja-build gcc libcurl4-openssl-dev libarchive-dev libsqlite3-dev libjansson-dev
```

### Para Fedora/RHEL/CentOS/AlmaLinux:
```bash
sudo dnf install meson ninja-build gcc libcurl-devel libarchive-devel sqlite-devel jansson-devel
```

### Para openSUSE:
```bash
sudo zypper install meson ninja gcc libcurl-devel libarchive-devel sqlite3-devel jansson-devel
```

---

## 📦 Instalación

### Método 1: Compilar desde fuente

```bash
# Clonar el repositorio
git clone https://github.com/apososgol1000-png/coco-package-manager.git
cd coco-package-manager

# Compilar
meson setup build
ninja -C build

# Instalar
sudo ninja -C build install
```

### Método 2: Desde repositorio binario

```bash
coco sync
coco install coco-package-manager
```

---

## 🚀 Uso Rápido

### Actualizar índice de repositorios
```bash
coco sync
```

### Instalar un paquete
```bash
coco install fastfetch
```

### Actualizar paquetes
```bash
coco upgrade                 # Actualiza todos los paquetes instalados
coco upgrade neofetch       # Actualiza solo el paquete especificado
```

### Buscar paquetes
```bash
coco search "nombre"
```

### Listar paquetes instalados
```bash
coco list
```

### Obtener información de un paquete
```bash
coco info fastfetch
```

### Eliminar un paquete
```bash
coco remove fastfetch
```

### Ver log de instalación
```bash
coco log fastfetch
```

---

## 📚 Comandos Disponibles

| Comando | Sintaxis | Descripción |
|---------|----------|-------------|
| `sync` | `coco sync` | Actualiza el índice de repositorios disponibles |
| `install` | `coco install <paquete>` | Instala o actualiza un paquete |
| `upgrade` | `coco upgrade [paquete]` | Actualiza paquetes (todos o uno específico) |
| `remove` | `coco remove <paquete>` | Desinstala un paquete |
| `search` | `coco search <término>` | Busca paquetes en el índice |
| `list` | `coco list` | Lista todos los paquetes instalados |
| `info` | `coco info <paquete>` | Muestra información detallada de un paquete |
| `log` | `coco log <paquete>` | Muestra el log de instalación de un paquete |

---

## 🏗️ Estructura del Repositorio

Para crear un repositorio compatible con Coco, organiza tus archivos de la siguiente manera:

```
mi-repositorio/
│
├── index.json                          # Índice central del repositorio (requerido)
│
├── fastfetch/
│   ├── fastfetch-1.0.0.tar.gz         # Paquete comprimido con binarios y hooks
│   ├── fastfetch-1.0.0.tar.gz.sha256  # Archivo de verificación SHA256
│   └── manifest.json                   # Metadatos del paquete
│
├── neofetch/
│   ├── neofetch-2.1.0.tar.gz
│   ├── neofetch-2.1.0.tar.gz.sha256
│   └── manifest.json
│
└── [más paquetes...]
```

### Contenido de `manifest.json`

```json
{
  "name": "fastfetch",
  "version": "1.0.0",
  "description": "Un fetcher de sistema rápido y minimalista",
  "checksum": "abc123def456...",
  "size": 1024000,
  "dependencies": [],
  "maintainer": "Tu Nombre <email@ejemplo.com>",
  "homepage": "https://github.com/fastfetch-cli/fastfetch"
}
```

### Contenido de `index.json`

```json
{
  "version": "1.0",
  "packages": {
    "fastfetch": {
      "latest": "1.0.0",
      "url": "https://repo.ejemplo.com/fastfetch/fastfetch-1.0.0.tar.gz"
    }
  }
}
```

---

## ⚙️ Configuración Avanzada

### Archivos de configuración

Coco almacena su configuración en:
- Linux: `~/.config/coco/config.json`

### Añadir un repositorio personalizado

```bash
coco config add-repo https://tu-repositorio.com
```

### Listar repositorios configurados

```bash
coco config list-repos
```

---

## ⚠️ Notas Técnicas

### Compatibilidad

| Sistema Operativo | Estado | Notas |
|-------------------|--------|-------|
| Debian | ✅ Soportado | Probado en Debian Stable |
| Ubuntu | ✅ Soportado | Probado en versiones LTS |
| Linux Mint | ✅ Soportado | Compatible total |
| openSUSE | ✅ Soportado | Desarrollo actual en openSUSE |
| Fedora/RHEL | ⚠️ Soporte limitado | Puede presentar incompatibilidades menores |
| Arch Linux | ❌ No soportado | Gestión de paquetes fundamentalmente diferente |

### Limitaciones Conocidas

- **Resolución de dependencias**: Actualmente Coco no resuelve conflictos de dependencias automáticamente. Esta funcionalidad se está desarrollando.
- **Rolling Release**: No es recomendado en distribuciones rolling release como Arch Linux, ya que los paquetes cambian frecuentemente.
- **Libsolv**: Se agregará próximamente para mejorar la resolución de dependencias.

### Desarrollo

- Inspirado en [Simple-Package-Manager (SPM)](https://github.com/simple-package-manager/simple-package-manager) - Licencia MIT
- Concepto inspirado en zypper
- Creado originalmente en Ubuntu 22.04
- Actualmente en desarrollo en openSUSE

---

## 🤝 Contribuir

Las contribuciones son bienvenidas. Si deseas colaborar:

1. **Fork** el proyecto
2. Crea una rama para tu feature (`git checkout -b feature/AmazingFeature`)
3. Commit tus cambios (`git commit -m 'Add some AmazingFeature'`)
4. Push a la rama (`git push origin feature/AmazingFeature`)
5. Abre un **Pull Request**

Para reportar bugs o solicitar features, [abre un issue](https://github.com/apososgol1000-png/coco-package-manager/issues).

---

## 📄 Licencia

Este proyecto está bajo la licencia **MIT**. Ver el archivo [LICENSE](LICENSE) para más detalles.

---

## 📞 Contacto

**Coconut Dynamics**

- 🌐 Sitio Web: [coconutdynamics.com](https://coconutdynamics.com)
- 📧 Email: [apososgol1000@gmail.com](mailto:apososgol1000@gmail.com)
- 🔗 Repositorio: [repo.coconutdynamics.com](https://repo.coconutdynamics.com)
- 💻 GitHub: [@apososgol1000-png](https://github.com/apososgol1000-png)

### Otros Proyectos

- 🖥️ [**CoconutWAY OS**](https://github.com/apososgol1000-png/coconutway-web) - El escritorio reinventado

---

<div align="center">

**¿Te gustó Coco? Considera darle una ⭐ en GitHub**

Hecho con ❤️ por Coconut Dynamics

</div>
