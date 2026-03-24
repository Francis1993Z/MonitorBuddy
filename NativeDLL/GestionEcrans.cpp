/*
 * GestionEcrans.cpp
 * -----------------
 * DLL native pour basculer entre un mode "TV" (écran unique)
 * et un mode "Bureau" (deux moniteurs LG côte à côte).
 *
 * DÉTECTION MATÉRIELLE via SetupAPI :
 *   Le code utilise SetupAPI (GUID_DEVCLASS_MONITOR) pour lire les vrais
 *   noms EDID et Hardware IDs de chaque moniteur physique, exactement comme
 *   le Gestionnaire de périphériques Windows. Il fait ensuite un "pont"
 *   (matching) entre ces infos et les adaptateurs d'affichage actifs
 *   (\\.\DISPLAY1, etc.) obtenus via EnumDisplayDevicesW.
 *
 * Stratégie Win32 (inchangée) :
 *   1. ChangeDisplaySettingsExW avec CDS_UPDATEREGISTRY | CDS_NORESET
 *      → enregistrer chaque changement SANS l'appliquer immédiatement.
 *   2. Un unique appel final ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL)
 *      → applique TOUS les changements d'un coup (évite le flickering).
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>       // Doit précéder devguid.h pour instancier les GUIDs
#include <setupapi.h>       // SetupDiGetClassDevs, SetupDiEnumDeviceInfo, etc.
#include <devguid.h>        // GUID_DEVCLASS_MONITOR
#include <string.h>
#include <stdio.h>
#include "GestionEcrans.h"

#pragma comment(lib, "setupapi.lib")   // Au cas où pas géré par CMake

/* ============================================================================
 * SECTION CONFIGURATION — Mots-clés pour identifier tes écrans
 * ============================================================================
 *
 * Le système lit le nom EDID (SPDRP_FRIENDLYNAME / SPDRP_DEVICEDESC)
 * et le Hardware ID (SPDRP_HARDWAREID, ex: "MONITOR\GSM5B08") de chaque
 * moniteur via SetupAPI, puis cherche ces mots-clés pour assigner les rôles.
 *
 * ──── MODIFIE CES LISTES SELON TON MATÉRIEL ────
 *
 * Pour trouver tes Hardware IDs, ouvre le Gestionnaire de périphériques,
 * section "Moniteurs", propriétés d'un écran → onglet "Détails" →
 * "Numéros d'identification du matériel".
 *
 * Exemples :
 *   - Moniteur LG 23" → HW ID contient "GSM" ou "LG", modèle "23EA53"
 *   - TV Hisense       → HW ID contient "HEC" ou "HISENSE"
 */
static const wchar_t* KEYWORDS_BUREAU[] = { L"LG", L"GSM", L"23EA53", NULL };
static const wchar_t* KEYWORDS_TV[]     = { L"HISENSE", L"HEC", NULL };

/* ============================================================================
 * STRUCTURES DE DONNÉES INTERNES
 * ============================================================================ */

// Rôle assigné à un écran
enum class Role { INCONNU, TV, BUREAU };

// Informations détectées pour un écran
struct InfoEcran
{
    wchar_t deviceName[32];     // ex: \\.\DISPLAY1 (adaptateur)
    wchar_t monitorName[256];   // ex: "Generic Monitor (LG FULL HD)" (nom EDID réel)
    wchar_t hardwareId[256];    // ex: "MONITOR\GSM5B08"
    int     nativeWidth;
    int     nativeHeight;
    Role    role;
    bool    actif;              // actuellement attaché au bureau ?
};

static const int MAX_ECRANS = 16;

/* ============================================================================
 * CACHE SetupAPI — Moniteurs physiques détectés
 * ============================================================================
 * On construit un cache de tous les moniteurs physiques connus de Windows
 * via SetupAPI, AVANT d'énumérer les adaptateurs d'affichage.
 * Ensuite, on fait le pont (matching) entre les deux.
 */

struct SetupApiMonitor
{
    wchar_t friendlyName[256];  // SPDRP_FRIENDLYNAME ou SPDRP_DEVICEDESC
    wchar_t hardwareId[256];    // SPDRP_HARDWAREID (ex: "MONITOR\GSM5B08")
    wchar_t instanceId[256];    // Device Instance ID complet
    wchar_t pnpPrefix[64];     // Les 2 premiers segments (ex: "MONITOR\GSM5B08")
};

static const int MAX_SETUP_MONITORS = 32;

/* ============================================================================
 * FONCTIONS UTILITAIRES DE DÉTECTION
 * ============================================================================ */

/*
 * ContientMotCle (insensible à la casse)
 * Retourne true si 'texte' contient 'motCle'.
 */
static bool ContientMotCle(const wchar_t* texte, const wchar_t* motCle)
{
    if (!texte || !motCle) return false;

    wchar_t bufTexte[512] = {};
    wchar_t bufMotCle[64] = {};

    for (int i = 0; i < 511 && texte[i]; i++)
        bufTexte[i] = (texte[i] >= L'A' && texte[i] <= L'Z')
                      ? texte[i] + 32 : texte[i];

    for (int i = 0; i < 63 && motCle[i]; i++)
        bufMotCle[i] = (motCle[i] >= L'A' && motCle[i] <= L'Z')
                       ? motCle[i] + 32 : motCle[i];

    return wcsstr(bufTexte, bufMotCle) != NULL;
}

/*
 * ContientUnDesMotsCles
 * Retourne true si 'texte' contient au moins un mot-clé de la liste.
 * La liste se termine par NULL.
 */
static bool ContientUnDesMotsCles(const wchar_t* texte, const wchar_t* const motsCles[])
{
    for (int i = 0; motsCles[i] != NULL; i++)
    {
        if (ContientMotCle(texte, motsCles[i]))
            return true;
    }
    return false;
}

/*
 * ExtrairePrefixPnP
 * Extrait les deux premiers segments d'un Device ID ou Instance ID.
 * Ex: "MONITOR\GSM5B08\{guid}\0001" → "MONITOR\GSM5B08"
 * Ex: "MONITOR\GSM5B08\5&abc&0&UID256" → "MONITOR\GSM5B08"
 */
static void ExtrairePrefixPnP(const wchar_t* fullId, wchar_t* prefix, int prefixSize)
{
    prefix[0] = L'\0';
    if (!fullId) return;

    // Chercher le 2ème backslash
    int bsCount = 0;
    int endPos = 0;

    for (int i = 0; fullId[i] && i < prefixSize - 1; i++)
    {
        if (fullId[i] == L'\\')
        {
            bsCount++;
            if (bsCount == 2)
            {
                endPos = i;
                break;
            }
        }
    }

    if (endPos > 0)
    {
        wcsncpy_s(prefix, prefixSize, fullId, endPos);
    }
    else
    {
        // Pas de 2ème backslash → copier tout
        wcsncpy_s(prefix, prefixSize, fullId, prefixSize - 1);
    }
}

/*
 * BuildSetupApiCache
 * ──────────────────
 * Utilise SetupAPI pour énumérer TOUS les moniteurs physiques connus
 * de Windows (classe GUID_DEVCLASS_MONITOR).
 *
 * Pour chaque moniteur trouvé, on récupère :
 *   - SPDRP_FRIENDLYNAME (nom convivial, ex: "Generic Monitor (LG FULL HD)")
 *   - SPDRP_DEVICEDESC   (fallback si pas de friendly name)
 *   - SPDRP_HARDWAREID   (ex: "MONITOR\GSM5B08")
 *   - Device Instance ID (pour le matching avec EnumDisplayDevices)
 *
 * Retourne le nombre de moniteurs trouvés.
 */
static int BuildSetupApiCache(SetupApiMonitor cache[], int maxMonitors)
{
    int count = 0;

    // Ouvrir l'ensemble des périphériques de la classe "Moniteur"
    // DIGCF_PRESENT = seulement les périphériques présents (connectés)
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(
        &GUID_DEVCLASS_MONITOR, NULL, NULL,
        DIGCF_PRESENT
    );

    if (hDevInfo == INVALID_HANDLE_VALUE)
        return 0;

    SP_DEVINFO_DATA devInfoData = {};
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0;
         count < maxMonitors && SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData);
         i++)
    {
        SetupApiMonitor& mon = cache[count];
        ZeroMemory(&mon, sizeof(SetupApiMonitor));

        // ── 1. Récupérer le nom convivial (SPDRP_FRIENDLYNAME) ──
        DWORD propType = 0;
        DWORD needed = 0;
        BOOL ok = SetupDiGetDeviceRegistryPropertyW(
            hDevInfo, &devInfoData,
            SPDRP_FRIENDLYNAME,
            &propType,
            (BYTE*)mon.friendlyName,
            sizeof(mon.friendlyName) - sizeof(wchar_t),
            &needed
        );

        // Fallback : SPDRP_DEVICEDESC si pas de friendly name
        if (!ok || mon.friendlyName[0] == L'\0')
        {
            SetupDiGetDeviceRegistryPropertyW(
                hDevInfo, &devInfoData,
                SPDRP_DEVICEDESC,
                &propType,
                (BYTE*)mon.friendlyName,
                sizeof(mon.friendlyName) - sizeof(wchar_t),
                &needed
            );
        }

        // ── 2. Récupérer le Hardware ID (SPDRP_HARDWAREID) ──
        // SPDRP_HARDWAREID retourne un REG_MULTI_SZ (plusieurs chaînes)
        // On ne prend que la première (la plus spécifique)
        wchar_t hwIdBuffer[512] = {};
        ok = SetupDiGetDeviceRegistryPropertyW(
            hDevInfo, &devInfoData,
            SPDRP_HARDWAREID,
            &propType,
            (BYTE*)hwIdBuffer,
            sizeof(hwIdBuffer) - sizeof(wchar_t),
            &needed
        );

        if (ok && hwIdBuffer[0] != L'\0')
        {
            // Première chaîne du MULTI_SZ
            wcsncpy_s(mon.hardwareId, hwIdBuffer, 255);
        }

        // ── 3. Récupérer le Device Instance ID ──
        SetupDiGetDeviceInstanceIdW(
            hDevInfo, &devInfoData,
            mon.instanceId,
            sizeof(mon.instanceId) / sizeof(wchar_t) - 1,
            &needed
        );

        // ── 4. Extraire le préfixe PnP pour le matching ──
        // Ex: "MONITOR\GSM5B08\5&abc&0&UID256" → "MONITOR\GSM5B08"
        ExtrairePrefixPnP(mon.instanceId, mon.pnpPrefix, 64);

        count++;
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return count;
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
 * AssignerRole
 * ────────────
 * Détermine le rôle d'un écran en cherchant les mots-clés dans
 * son nom convivial ET son Hardware ID (les deux sources).
 *
 * Priorité : BUREAU d'abord, puis TV, sinon INCONNU → défaut TV.
 */
static Role AssignerRole(const wchar_t* friendlyName, const wchar_t* hardwareId)
{
    // Vérifier BUREAU dans le nom OU dans le HW ID
    if (ContientUnDesMotsCles(friendlyName, KEYWORDS_BUREAU) ||
        ContientUnDesMotsCles(hardwareId, KEYWORDS_BUREAU))
        return Role::BUREAU;

    // Vérifier TV dans le nom OU dans le HW ID
    if (ContientUnDesMotsCles(friendlyName, KEYWORDS_TV) ||
        ContientUnDesMotsCles(hardwareId, KEYWORDS_TV))
        return Role::TV;

    // Aucun match → par défaut TV (la TV a souvent un nom générique)
    return Role::TV;
}

/* ============================================================================
 * ScannerEcrans — LE PONT (MATCHING) entre SetupAPI et EnumDisplayDevices
 * ============================================================================
 *
 * Algorithme en 3 phases :
 *
 * Phase 1 : Construire le cache SetupAPI
 *   → Liste de tous les moniteurs physiques avec noms EDID et HW IDs.
 *
 * Phase 2 : Énumérer les adaptateurs (EnumDisplayDevicesW)
 *   → Pour chaque \\.\DISPLAYn, récupérer le DeviceID du moniteur branché.
 *
 * Phase 3 : Matching
 *   → Le DeviceID du moniteur (EnumDisplayDevices niveau 2) ressemble à :
 *       "MONITOR\GSM5B08\{4d36e96e-e325-11ce-bfc1-08002be10318}\0001"
 *   → Le Instance ID du moniteur (SetupAPI) ressemble à :
 *       "MONITOR\GSM5B08\5&12345678&0&UID256"
 *   → Les deux partagent le préfixe "MONITOR\GSM5B08" (2 premiers segments).
 *   → On matche sur ce préfixe pour trouver le bon moniteur SetupAPI.
 */
static int ScannerEcrans(InfoEcran ecrans[], int maxEcrans)
{
    // ── Phase 1 : Cache SetupAPI ──
    SetupApiMonitor setupCache[MAX_SETUP_MONITORS];
    int nbSetup = BuildSetupApiCache(setupCache, MAX_SETUP_MONITORS);

    int count = 0;

    // ── Phase 2 : Énumérer les adaptateurs ──
    for (DWORD i = 0; count < maxEcrans; i++)
    {
        DISPLAY_DEVICE adapter = {};
        adapter.cb = sizeof(DISPLAY_DEVICE);

        if (!EnumDisplayDevicesW(NULL, i, &adapter, 0))
            break;

        InfoEcran& ecran = ecrans[count];
        ZeroMemory(&ecran, sizeof(InfoEcran));

        // Copier le nom de l'adaptateur (\\.\DISPLAY1, etc.)
        wcsncpy_s(ecran.deviceName, adapter.DeviceName, 31);

        // L'écran est-il actuellement actif ?
        ecran.actif = (adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;

        // Niveau 2 : Récupérer le DeviceID du moniteur branché sur cet adaptateur
        DISPLAY_DEVICE monitor = {};
        monitor.cb = sizeof(DISPLAY_DEVICE);

        wchar_t enumPrefix[64] = {};

        if (EnumDisplayDevicesW(adapter.DeviceName, 0, &monitor, 0))
        {
            // monitor.DeviceID → ex: "MONITOR\GSM5B08\{guid}\0001"
            ExtrairePrefixPnP(monitor.DeviceID, enumPrefix, 64);
        }

        // ── Phase 3 : Matching avec le cache SetupAPI ──
        bool matched = false;

        if (enumPrefix[0] != L'\0')
        {
            for (int j = 0; j < nbSetup; j++)
            {
                // Comparer les préfixes PnP (insensible à la casse)
                if (_wcsicmp(enumPrefix, setupCache[j].pnpPrefix) == 0)
                {
                    // MATCH TROUVÉ ! Copier les infos SetupAPI
                    wcsncpy_s(ecran.monitorName, setupCache[j].friendlyName, 255);
                    wcsncpy_s(ecran.hardwareId, setupCache[j].hardwareId, 255);
                    matched = true;
                    break;
                    // Note : pour 2 moniteurs identiques (même PnP ID),
                    // le premier match du cache est utilisé pour les deux.
                    // C'est OK car ils auront le même rôle (même modèle).
                }
            }
        }

        // Fallback si pas de match SetupAPI
        if (!matched)
        {
            if (monitor.DeviceString[0] != L'\0')
                wcsncpy_s(ecran.monitorName, monitor.DeviceString, 255);
            else
                wcsncpy_s(ecran.monitorName, adapter.DeviceString, 255);

            // Utiliser le DeviceID comme hardware ID de fallback
            if (monitor.DeviceID[0] != L'\0')
                wcsncpy_s(ecran.hardwareId, monitor.DeviceID, 255);
        }

        // Détecter la résolution native
        DetecterResolutionNative(ecran.deviceName,
                                &ecran.nativeWidth, &ecran.nativeHeight);

        // Assigner le rôle en se basant sur le vrai nom ET le Hardware ID
        ecran.role = AssignerRole(ecran.monitorName, ecran.hardwareId);

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
        if (ecrans[i].role == Role::BUREAU && ecrans[i].actif)
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
        if (ecrans[i].role == Role::TV && ecrans[i].actif)
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
            L"%s | %s | %s | %dx%d | %s | %s\n",
            ecrans[i].deviceName,
            ecrans[i].monitorName,
            ecrans[i].hardwareId,
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
