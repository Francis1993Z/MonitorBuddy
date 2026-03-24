cmake_minimum_required(VERSION 3.15)
project(GestionEcrans LANGUAGES CXX)

# ── DLL partagée ──
add_library(GestionEcrans SHARED GestionEcrans.cpp)

# ── Définir la macro d'export ──
target_compile_definitions(GestionEcrans PRIVATE GESTIONECRANS_EXPORTS)

# ── Lier user32.lib (CCD API: QueryDisplayConfig, SetDisplayConfig, etc.) ──
target_link_libraries(GestionEcrans PRIVATE user32)

# ── Propriétés de sortie ──
set_target_properties(GestionEcrans PROPERTIES
    OUTPUT_NAME "GestionEcrans"
    PREFIX ""                           # Pas de préfixe "lib" sous Windows
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
