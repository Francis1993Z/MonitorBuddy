/*
 * GestionEcrans.cpp
 * -----------------
 * DLL native pour basculer entre un mode "TV" (écran unique)
 * et un mode "Bureau" (deux moniteurs LG côte à côte).
 *
 * AUTO-DÉTECTION : Le code scanne les écrans via EnumDisplayDevices,
 * lit le nom EDID de chaque moniteur, et assigne automatiquement
 * les rôles (TV vs Bureau) en cherchant des mots-clés dans les noms.
 *
 * Stratégie Win32 :
 *   1. EnumDisplayDevicesW → identifier les écrans + noms EDID.
 *   2. EnumDisplaySettingsW → lire résolution native de chaque écran.
 *   3. ChangeDisplaySettingsExW avec CDS_UPDATEREGISTRY | CDS_NORESET
 *      → enregistrer chaque changement SANS l'appliquer immédiatement.
 *   4. Un unique appel final ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL)
 *      → applique TOUS les changements d'un coup (évite le flickering).
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "GestionEcrans.h"

/* ============================================================================
 * SECTION CONFIGURATION — Mots-clés pour identifier tes écrans
 * ============================================================================
 *
 * Le système scanne le nom EDID de chaque moniteur (le "DeviceString" retourné
 * par EnumDisplayDevicesW au deuxième niveau). Si le nom contient un des
 * mots-clés ci-dessous, il est classé comme moniteur de bureau.
 * Tout écran qui ne matche PAS est considéré comme la TV.
 *
 * Exemples de noms EDID courants :
 *   - "LG FULL HD"  /  "LG IPS FULLHD"  → contient "LG"
 *   - "SONY TV"  /  "Samsung"            → ne contient PAS "LG" → TV
 *   - "Generic PnP Monitor"              → attention, nom générique
 *
 * ──── MODIFIE CES MOTS-CLÉS SELON TON MATÉRIEL ────
 */
static const wchar_t* KEYWORD_BUREAU = L"LG";

/* ============================================================================
 * STRUCTURES DE DONNÉES INTERNES
 * ============================================================================ */

// Rôle assigné à un écran
enum class Role { INCONNU, TV, BUREAU };

// Informations détectées pour un écran
struct InfoEcran
{
    wchar_t deviceName[32];     // ex: \\.\DISPLAY1
    wchar_t monitorName[128];   // ex: "LG FULL HD" (nom EDID)
    int     nativeWidth;
    int     nativeHeight;
    Role    role;
    bool    actif;              // actuellement attaché au bureau ?
};

static const int MAX_ECRANS = 16;

/* ============================================================================
 * FONCTIONS DE DÉTECTION
 * ============================================================================ */

/*
 * ContientMotCle (insensible à la casse)
 * Retourne true si 'texte' contient 'motCle'.
 */
static bool ContientMotCle(const wchar_t* texte, const wchar_t* motCle)
{
    if (!texte || !motCle) return false;

    // Copie locale en minuscules pour comparaison insensible à la casse
    wchar_t bufTexte[256] = {};
    wchar_t bufMotCle[64] = {};

    for (int i = 0; i < 255 && texte[i]; i++)
        bufTexte[i] = (texte[i] >= L'A' && texte[i] <= L'Z')
                      ? texte[i] + 32 : texte[i];

    for (int i = 0; i < 63 && motCle[i]; i++)
        bufMotCle[i] = (motCle[i] >= L'A' && motCle[i] <= L'Z')
                       ? motCle[i] + 32 : motCle[i];

    return wcsstr(bufTexte, bufMotCle) != NULL;
}

/*
 * DetecterResolutionNative
 * Parcourt tous les modes disponibles et retourne la résolution la plus haute.
 */
static void DetecterResolutionNative(const wchar_t* deviceName,
                                     int* outWidth, int* outHeight)
{
    *outWidth  = 0;
    *outHeight = 0;

    DEVMODE dm = {};
    dm.dmSize = sizeof(DEVMODE);

    // D'abord, tenter le mode courant ou registre
    if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmPelsWidth > 0)
    {
        *outWidth  = dm.dmPelsWidth;
        *outHeight = dm.dmPelsHeight;
        return;
    }

    if (EnumDisplaySettingsW(deviceName, ENUM_REGISTRY_SETTINGS, &dm) &&
        dm.dmPelsWidth > 0)
    {
        *outWidth  = dm.dmPelsWidth;
        *outHeight = dm.dmPelsHeight;
        return;
    }

    // Sinon, parcourir tous les modes et prendre le plus grand
    for (DWORD i = 0; ; i++)
    {
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(DEVMODE);

        if (!EnumDisplaySettingsW(deviceName, i, &dm))
            break;

        int pixels = dm.dmPelsWidth * dm.dmPelsHeight;
        if (pixels > (*outWidth) * (*outHeight))
        {
            *outWidth  = dm.dmPelsWidth;
            *outHeight = dm.dmPelsHeight;
        }
    }
}

/*
 * ScannerEcrans
 * Énumère tous les écrans connectés, lit leur nom EDID,
 * détecte leur résolution native, et assigne les rôles.
 * Retourne le nombre d'écrans trouvés.
 */
static int ScannerEcrans(InfoEcran ecrans[], int maxEcrans)
{
    int count = 0;

    for (DWORD i = 0; count < maxEcrans; i++)
    {
        // Niveau 1 : Énumérer les adaptateurs (\\.\DISPLAY1, etc.)
        DISPLAY_DEVICE adapter = {};
        adapter.cb = sizeof(DISPLAY_DEVICE);

        if (!EnumDisplayDevicesW(NULL, i, &adapter, 0))
            break;

        // Ignorer les adaptateurs non connectés
        if (!(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
            !(adapter.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER))
        {
            // L'adaptateur existe mais n'est pas actif — on le garde quand même
            // car il peut être détaché mais physiquement connecté
        }

        InfoEcran& ecran = ecrans[count];
        ZeroMemory(&ecran, sizeof(InfoEcran));

        // Copier le nom du device (\\.\DISPLAY1, etc.)
        wcsncpy_s(ecran.deviceName, adapter.DeviceName, 31);

        // L'écran est-il actuellement actif sur le bureau ?
        ecran.actif = (adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;

        // Niveau 2 : Énumérer le moniteur branché sur cet adaptateur
        DISPLAY_DEVICE monitor = {};
        monitor.cb = sizeof(DISPLAY_DEVICE);

        if (EnumDisplayDevicesW(adapter.DeviceName, 0, &monitor, 0))
        {
            // monitor.DeviceString contient le nom EDID (ex: "LG FULL HD")
            wcsncpy_s(ecran.monitorName, monitor.DeviceString, 127);
        }
        else
        {
            // Pas de moniteur branché → utiliser le nom de l'adaptateur
            wcsncpy_s(ecran.monitorName, adapter.DeviceString, 127);
        }

        // Détecter la résolution native
        DetecterResolutionNative(ecran.deviceName,
                                &ecran.nativeWidth, &ecran.nativeHeight);

        // Si résolution 0 et pas actif, l'écran est probablement déconnecté
        // On le garde quand même dans la liste pour le diagnostic

        // Assigner le rôle basé sur le mot-clé
        if (ContientMotCle(ecran.monitorName, KEYWORD_BUREAU))
            ecran.role = Role::BUREAU;
        else
            ecran.role = Role::TV;

        count++;
    }

    return count;
}

/* ============================================================================
 * FONCTIONS UTILITAIRES INTERNES (Win32)
 * ============================================================================ */

static bool DetacherEcran(const wchar_t* nomDisplay)
{
    DEVMODE dm = {};
    dm.dmSize = sizeof(DEVMODE);

    EnumDisplaySettingsW(nomDisplay, ENUM_CURRENT_SETTINGS, &dm);

    dm.dmPelsWidth  = 0;
    dm.dmPelsHeight = 0;
    dm.dmPosition.x = 0;
    dm.dmPosition.y = 0;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;

    LONG result = ChangeDisplaySettingsExW(
        nomDisplay, &dm, NULL,
        CDS_UPDATEREGISTRY | CDS_NORESET, NULL
    );

    return (result == DISP_CHANGE_SUCCESSFUL);
}

static bool ActiverEcranPrincipal(const wchar_t* nomDisplay,
                                  int largeur, int hauteur)
{
    DEVMODE dm = {};
    dm.dmSize = sizeof(DEVMODE);

    if (!EnumDisplaySettingsW(nomDisplay, ENUM_CURRENT_SETTINGS, &dm))
        if (!EnumDisplaySettingsW(nomDisplay, ENUM_REGISTRY_SETTINGS, &dm))
        {
            ZeroMemory(&dm, sizeof(dm));
            dm.dmSize = sizeof(DEVMODE);
        }

    dm.dmPelsWidth  = static_cast<DWORD>(largeur);
    dm.dmPelsHeight = static_cast<DWORD>(hauteur);
    dm.dmPosition.x = 0;
    dm.dmPosition.y = 0;
    dm.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;

    LONG result = ChangeDisplaySettingsExW(
        nomDisplay, &dm, NULL,
        CDS_UPDATEREGISTRY | CDS_NORESET | CDS_SET_PRIMARY, NULL
    );

    return (result == DISP_CHANGE_SUCCESSFUL);
}

static bool ActiverEcranSecondaire(const wchar_t* nomDisplay,
                                   int posX, int posY,
                                   int largeur, int hauteur)
{
    DEVMODE dm = {};
    dm.dmSize = sizeof(DEVMODE);

    if (!EnumDisplaySettingsW(nomDisplay, ENUM_CURRENT_SETTINGS, &dm))
        if (!EnumDisplaySettingsW(nomDisplay, ENUM_REGISTRY_SETTINGS, &dm))
        {
            ZeroMemory(&dm, sizeof(dm));
            dm.dmSize = sizeof(DEVMODE);
        }

    dm.dmPelsWidth  = static_cast<DWORD>(largeur);
    dm.dmPelsHeight = static_cast<DWORD>(hauteur);
    dm.dmPosition.x = posX;
    dm.dmPosition.y = posY;
    dm.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;

    LONG result = ChangeDisplaySettingsExW(
        nomDisplay, &dm, NULL,
        CDS_UPDATEREGISTRY | CDS_NORESET, NULL
    );

    return (result == DISP_CHANGE_SUCCESSFUL);
}

static void AppliquerChangements()
{
    ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
}

/* ============================================================================
 * FONCTIONS EXPORTÉES
 * ============================================================================ */

/*
 * ActiverModeTV
 * 1. Scanner les écrans
 * 2. Détacher tous les écrans de rôle BUREAU
 * 3. Activer le premier écran de rôle TV comme principal (0,0)
 * 4. Appliquer d'un seul coup
 */
GESTIONECRANS_API bool ActiverModeTV()
{
    InfoEcran ecrans[MAX_ECRANS];
    int nb = ScannerEcrans(ecrans, MAX_ECRANS);

    if (nb == 0) return false;

    bool ok = true;
    bool tvTrouvee = false;

    // Étape 1 — Détacher tous les écrans BUREAU
    for (int i = 0; i < nb; i++)
    {
        if (ecrans[i].role == Role::BUREAU)
        {
            if (!DetacherEcran(ecrans[i].deviceName))
                ok = false;
        }
    }

    // Étape 2 — Activer le premier écran TV trouvé comme principal
    for (int i = 0; i < nb; i++)
    {
        if (ecrans[i].role == Role::TV && ecrans[i].nativeWidth > 0)
        {
            if (!ActiverEcranPrincipal(ecrans[i].deviceName,
                                       ecrans[i].nativeWidth,
                                       ecrans[i].nativeHeight))
                ok = false;

            tvTrouvee = true;
            break;
        }
    }

    if (!tvTrouvee) ok = false;

    // Étape 3 — Appliquer
    AppliquerChangements();

    return ok;
}

/*
 * ActiverModeBureau
 * 1. Scanner les écrans
 * 2. Détacher tous les écrans de rôle TV
 * 3. Activer le premier LG comme principal (0,0)
 * 4. Activer le second LG à droite (offsetX = résolution du premier)
 * 5. Appliquer d'un seul coup
 */
GESTIONECRANS_API bool ActiverModeBureau()
{
    InfoEcran ecrans[MAX_ECRANS];
    int nb = ScannerEcrans(ecrans, MAX_ECRANS);

    if (nb == 0) return false;

    bool ok = true;

    // Collecter les écrans BUREAU avec résolution valide
    InfoEcran* bureaux[MAX_ECRANS];
    int nbBureaux = 0;

    for (int i = 0; i < nb; i++)
    {
        if (ecrans[i].role == Role::BUREAU && ecrans[i].nativeWidth > 0)
            bureaux[nbBureaux++] = &ecrans[i];
    }

    if (nbBureaux == 0) return false;

    // Étape 1 — Détacher tous les écrans TV
    for (int i = 0; i < nb; i++)
    {
        if (ecrans[i].role == Role::TV)
        {
            if (!DetacherEcran(ecrans[i].deviceName))
                ok = false;
        }
    }

    // Étape 2 — Premier LG → écran principal à (0,0)
    if (!ActiverEcranPrincipal(bureaux[0]->deviceName,
                               bureaux[0]->nativeWidth,
                               bureaux[0]->nativeHeight))
        ok = false;

    // Étape 3 — LG suivants → positionnés côte à côte vers la droite
    int offsetX = bureaux[0]->nativeWidth;

    for (int i = 1; i < nbBureaux; i++)
    {
        if (!ActiverEcranSecondaire(bureaux[i]->deviceName,
                                    offsetX, 0,
                                    bureaux[i]->nativeWidth,
                                    bureaux[i]->nativeHeight))
            ok = false;

        offsetX += bureaux[i]->nativeWidth;
    }

    // Étape 4 — Appliquer
    AppliquerChangements();

    return ok;
}

/*
 * ObtenirInfoEcrans
 * Fonction de diagnostic : remplit le buffer avec un rapport lisible
 * de tous les écrans détectés, leur nom EDID, résolution et rôle.
 */
GESTIONECRANS_API void ObtenirInfoEcrans(wchar_t* buffer, int tailleMax)
{
    if (!buffer || tailleMax <= 0) return;
    buffer[0] = L'\0';

    InfoEcran ecrans[MAX_ECRANS];
    int nb = ScannerEcrans(ecrans, MAX_ECRANS);

    wchar_t ligne[512];
    int pos = 0;

    for (int i = 0; i < nb; i++)
    {
        const wchar_t* roleStr = L"???";
        switch (ecrans[i].role)
        {
            case Role::TV:      roleStr = L"TV";      break;
            case Role::BUREAU:  roleStr = L"BUREAU";  break;
            default:            roleStr = L"INCONNU"; break;
        }

        _snwprintf_s(ligne, 511, _TRUNCATE,
            L"%s | %s | %dx%d | %s | %s\n",
            ecrans[i].deviceName,
            ecrans[i].monitorName,
            ecrans[i].nativeWidth,
            ecrans[i].nativeHeight,
            roleStr,
            ecrans[i].actif ? L"Actif" : L"Inactif"
        );

        int len = (int)wcslen(ligne);
        if (pos + len >= tailleMax - 1)
            break;

        wcscat_s(buffer, tailleMax, ligne);
        pos += len;
    }
}

/* ============================================================================
 * POINT D'ENTRÉE DLL
 * ============================================================================ */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    (void)hModule;
    (void)ul_reason_for_call;
    (void)lpReserved;
    return TRUE;
}
