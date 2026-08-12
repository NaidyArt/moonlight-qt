Moonlight AVSampleBuffer para MacBook Pro M2 Max
================================================

Perfil validado: 3456x2160, 120 FPS, HEVC 10-bit HDR.

Instalación
-----------
1. Cierra cualquier Moonlight abierto.
2. Abre el DMG.
3. Arrastra "Moonlight AVSampleBuffer.app" a Aplicaciones.
4. Inicia esa app. No reemplaza /Applications/Moonlight.app.

La primera apertura puede requerir clic derecho > Abrir porque este candidato
está firmado ad-hoc y no notarizado. No desactives Gatekeeper globalmente.

Comportamiento
--------------
- AVSampleBufferDisplayLayer es el renderer predeterminado en macOS.
- La selección sigue disponible y se guarda desde Ajustes > Renderer.
- Se conserva el nombre de QSettings de Moonlight para reutilizar hosts,
  emparejamientos y el perfil 3456x2160/120/HEVC/HDR ya configurado.
- Solo la clave de renderer es propia de esta variante: una selección previa
  Auto/Metal de Moonlight oficial no cambia este valor ni es modificada.
- El bundle ID y el nombre de la app son distintos, por lo que puede convivir
  con Moonlight oficial.
- No contiene el experimento Metal asíncrono.
- Mientras el overlay de estadísticas esté visible, guarda JSONL privado a
  1 Hz en ~/Library/Logs/Moonlight AVSampleBuffer/. Al ocultarlo, deja de
  aceptar muestras inmediatamente; cualquier I/O ya iniciada y el
  flush/fsync/cierre terminan en el worker, sin bloquear input ni apagado. Se
  puede desactivar en Ajustes sin desactivar el overlay.

Verificación en uso
-------------------
Abre las estadísticas durante un stream y confirma que el log muestra
exactamente:
Renderer 'VideoToolbox (AVSampleBufferDisplayLayer)' chosen

Rollback
--------
Ejecuta "Desinstalar Moonlight AVSampleBuffer.command" desde el paquete de
entrega. Mueve únicamente esta app a la Papelera tras validar su bundle ID.
También puedes borrarla manualmente y volver a abrir Moonlight oficial.

Firma y notarización
--------------------
La app tiene firma ad-hoc verificada por CI. No está notarizada: para ello se
necesita un certificado Developer ID Application y credenciales privadas de
Apple, que no se han proporcionado al build.
