/*
 * GestionEcrans.cpp
 * -----------------
 * DLL native pour basculer entre un mode "TV" (écran unique)
 * et un mode "Bureau" (deux moniteurs LG côte à côte).
 *
 * API MODERNE CCD (Connecting and Configuring Displays) :
 *   Abandonne l'ancienne API ChangeDisplaySettingsExW au profit de :
 *     1. GetDisplayConfigBufferSizes + QueryDisplayConfig(QDC_ALL_PATHS)
 *        -> lire TOUTE la topologie connue du systeme.
 *     2. DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME)
 *        -> identifier chaque cible (nom EDID, device path).
 *     3. Modifier les flags DISPLAYCONFIG_PATH_ACTIVE sur chaque chemin.
 *     4. SetDisplayConfig(SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
 *        SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE)
 *        -> appliquer la nouvelle topologie d'un seul coup.
 *
 *   Cible : Windows 10 et superieur.
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <vector>
#include "GestionEcrans.h"

/* ============================================================================
 * SECTION CONFIGURATION
 * ============================================================================
 *
 * Le systeme lit le nom convivial (monitorFriendlyDeviceName) et le
 * chemin materiel (monitorDevicePath) de chaque cible CCD.
 * Le device path contient le PnP ID (ex: "HEC81C9", "GSM59A8").
 *
 * ---- MODIFIE CES LISTES SELON TON MATERIEL ----
 */
static const wchar_t* KEYWORDS_BUREAU[] = { L"LG", L"GSM", L"23EA53", NULL };
static const wchar_t* KEYWORDS_TV[]     = { L"HISENSE", L"HEC", NULL };

enum class Role { INCONNU, TV, BUREAU };

/* ============================================================================
 * LOGGING (operations.log a cote de la DLL)
 * ============================================================================ */

static FILE* g_logFile = NULL;

static void LogOuvrir()
{
    if (g_logFile) return;
    wchar_t dllPath[MAX_PATH] = {};
    HMODULE hSelf = NULL;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&LogOuvrir, &hSelf);
    GetModuleFileNameW(hSelf, dllPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(dllPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    wcscat_s(dllPath, MAX_PATH, L"operations.log");
    _wfopen_s(&g_logFile, dllPath, L"w, ccs=UTF-8");
}

static void LogFermer()
{
    if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
}

static void Log(const wchar_t* fmt, ...)
{
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfwprintf(g_logFile, fmt, args);
    va_end(args);
    fflush(g_logFile);
}

/* ============================================================================
 * KEYWORD MATCHING (insensible a la casse)
 * ============================================================================ */

static bool ContientMotCle(const wchar_t* texte, const wchar_t* motCle)
{
    if (!texte || !motCle) return false;
    wchar_t bufTexte[512] = {};
    wchar_t bufMotCle[64] = {};
    for (int i = 0; i < 511 && texte[i]; i++)
        bufTexte[i] = towlower(texte[i]);
    for (int i = 0; i < 63 && motCle[i]; i++)
        bufMotCle[i] = towlower(motCle[i]);
    return wcsstr(bufTexte, bufMotCle) != NULL;
}

static bool ContientUnDesMotsCles(const wchar_t* texte,
                                   const wchar_t* const motsCles[])
{
    for (int i = 0; motsCles[i] != NULL; i++)
        if (ContientMotCle(texte, motsCles[i]))
            return true;
    return false;
}

/* ============================================================================
 * CCD — IDENTIFICATION DES CIBLES
 * ============================================================================ */

static bool ObtenirNomCible(
    const DISPLAYCONFIG_PATH_INFO& path,
    DISPLAYCONFIG_TARGET_DEVICE_NAME& outName)
{
    ZeroMemory(&outName, sizeof(outName));
    outName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    outName.header.size = sizeof(outName);
    outName.header.adapterId = path.targetInfo.adapterId;
    outName.header.id = path.targetInfo.id;
    return (DisplayConfigGetDeviceInfo(&outName.header) == ERROR_SUCCESS);
}

static Role DeterminerRole(const wchar_t* friendlyName,
                            const wchar_t* devicePath)
{
    if (ContientUnDesMotsCles(friendlyName, KEYWORDS_BUREAU) ||
        ContientUnDesMotsCles(devicePath, KEYWORDS_BUREAU))
        return Role::BUREAU;

    if (ContientUnDesMotsCles(friendlyName, KEYWORDS_TV) ||
        ContientUnDesMotsCles(devicePath, KEYWORDS_TV))
        return Role::TV;

    return Role::INCONNU;
}

/* ============================================================================
 * CCD — LECTURE DE LA TOPOLOGIE
 * ============================================================================ */

static bool LireTopologie(
    std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    UINT32 numPaths = 0, numModes = 0;

    LONG result = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes);
    if (result != ERROR_SUCCESS)
    {
        Log(L"  GetDisplayConfigBufferSizes ECHEC: %d\n", result);
        return false;
    }

    paths.resize(numPaths);
    modes.resize(numModes);

    result = QueryDisplayConfig(
        QDC_ALL_PATHS,
        &numPaths, paths.data(),
        &numModes, modes.data(),
        nullptr
    );

    if (result != ERROR_SUCCESS)
    {
        Log(L"  QueryDisplayConfig ECHEC: %d\n", result);
        return false;
    }

    paths.resize(numPaths);
    modes.resize(numModes);

    Log(L"  Topologie lue: %u paths, %u modes\n", numPaths, numModes);
    return true;
}

/* ============================================================================
 * CCD — APPLICATION DE LA TOPOLOGIE
 * ============================================================================ */

static bool AppliquerTopologie(
    std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    LONG result = SetDisplayConfig(
        (UINT32)paths.size(), paths.data(),
        (UINT32)modes.size(), modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
        SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE
    );

    Log(L"  SetDisplayConfig -> %d (%s)\n", result,
        result == ERROR_SUCCESS ? L"OK" : L"ECHEC");

    return (result == ERROR_SUCCESS);
}

/* ============================================================================
 * HELPERS — Deduplication des cibles (adapterId + target ID)
 * ============================================================================
 *
 * Avec QDC_ALL_PATHS, Windows retourne TOUS les chemins possibles
 * (source -> cible). Il peut y avoir plusieurs chemins vers la meme cible
 * physique. On ne doit activer qu'UN SEUL chemin par cible unique.
 */

static bool LuidEqual(LUID a, LUID b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

struct TargetId
{
    LUID   adapterId;
    UINT32 id;
};

static bool TargetDejaVu(const std::vector<TargetId>& liste,
                          LUID adapterId, UINT32 id)
{
    for (const auto& t : liste)
        if (LuidEqual(t.adapterId, adapterId) && t.id == id)
            return true;
    return false;
}

/* ============================================================================
 * LOGIQUE CENTRALE — BasculerMode
 * ============================================================================
 *
 * Algorithme en 3 etapes :
 *
 *   1. LIRE la topologie complete (QDC_ALL_PATHS)
 *
 *   2. MODIFIER les flags DISPLAYCONFIG_PATH_ACTIVE :
 *      - Cibles TV ou BUREAU -> activer ou desactiver selon le mode.
 *      - Cibles INCONNU (Meta VR, GPU orphelin) -> NE PAS TOUCHER.
 *      - Deduplication : un seul chemin actif par cible unique.
 *      - On prefere les chemins deja actifs (modes valides en memoire).
 *
 *   3. APPLIQUER via SetDisplayConfig avec SDC_ALLOW_CHANGES
 *      -> Windows gere coordonnees, modes et resolutions tout seul.
 *      -> Plus AUCUN conflit de coordonnees !
 */

static bool BasculerMode(bool modeTv)
{
    LogOuvrir();
    Log(L"\n========== %s ==========\n",
        modeTv ? L"ActiverModeTV()" : L"ActiverModeBureau()");

    // -- Etape 1 : Lire la topologie complete --
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;

    if (!LireTopologie(paths, modes))
    {
        LogFermer();
        return false;
    }

    // -- Etape 2 : Classifier chaque chemin --
    struct PathInfo
    {
        size_t index;
        Role   role;
        bool   wasActive;
        LUID   adapterId;
        UINT32 targetId;
        LUID   sourceAdapterId;
        UINT32 sourceId;
    };

    std::vector<PathInfo> infos;

    for (size_t i = 0; i < paths.size(); i++)
    {
        auto& path = paths[i];

        if (!path.targetInfo.targetAvailable)
            continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName;
        if (!ObtenirNomCible(path, targetName))
            continue;

        Role role = DeterminerRole(
            targetName.monitorFriendlyDeviceName,
            targetName.monitorDevicePath
        );

        bool wasActive = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;

        Log(L"  Path[%zu]: \"%s\" | %s | %s\n",
            i,
            targetName.monitorFriendlyDeviceName,
            wasActive ? L"ACTIF" : L"inactif",
            role == Role::TV     ? L"TV" :
            role == Role::BUREAU ? L"BUREAU" : L"INCONNU"
        );

        infos.push_back({i, role, wasActive,
                          path.targetInfo.adapterId,
                          path.targetInfo.id,
                          path.sourceInfo.adapterId,
                          path.sourceInfo.id});
    }

    // -- Etape 2a : Desactiver tous les chemins TV/BUREAU --
    // (les INCONNU gardent leur etat actuel, on ne les touche PAS)
    for (auto& info : infos)
    {
        if (info.role == Role::TV || info.role == Role::BUREAU)
            paths[info.index].flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
    }

    // -- Etape 2b : Activer les chemins voulus --
    //
    // BUREAU ETENDU (pas clone) :
    //   Deux chemins CCD qui partagent la MEME source GPU → clone (duplique).
    //   Deux chemins CCD avec des sources DIFFERENTES → extend (etendu).
    //   On doit donc s'assurer que chaque cible BUREAU utilise une source unique.
    //
    // Strategie :
    //   - Tracker les sources deja utilisees (usedSources)
    //   - Pour chaque cible, preferer un chemin dont la source est libre
    //   - En mode TV (1 seul ecran), pas de contrainte de source

    std::vector<TargetId> activated;
    std::vector<TargetId> usedSources;  // sources GPU deja assignees

    // Fonction lambda : verifier si une source est deja utilisee
    auto sourceDejaUtilisee = [&](LUID sAdapterId, UINT32 sId) -> bool {
        for (const auto& s : usedSources)
            if (LuidEqual(s.adapterId, sAdapterId) && s.id == sId)
                return true;
        return false;
    };

    // Collecter les cibles uniques a activer
    struct CibleVoulue {
        LUID   adapterId;
        UINT32 targetId;
    };
    std::vector<CibleVoulue> ciblesVoulues;

    for (auto& info : infos)
    {
        bool shouldActivate = (modeTv  && info.role == Role::TV) ||
                              (!modeTv && info.role == Role::BUREAU);

        if (shouldActivate &&
            !TargetDejaVu(activated, info.adapterId, info.targetId))
        {
            ciblesVoulues.push_back({info.adapterId, info.targetId});
            activated.push_back({info.adapterId, info.targetId});
        }
    }

    // Pour chaque cible voulue, trouver le meilleur chemin
    // Priorite : (1) deja actif + source libre, (2) inactif + source libre,
    //            (3) deja actif (meme si source prise), (4) inactif
    activated.clear();

    for (auto& cible : ciblesVoulues)
    {
        size_t bestIdx = (size_t)-1;
        int    bestScore = -1;

        for (auto& info : infos)
        {
            if (!LuidEqual(info.adapterId, cible.adapterId) ||
                info.targetId != cible.targetId)
                continue;

            bool srcLibre = !sourceDejaUtilisee(info.sourceAdapterId, info.sourceId);
            int score = 0;
            if (info.wasActive && srcLibre)  score = 4;  // ideal
            else if (!info.wasActive && srcLibre) score = 3;
            else if (info.wasActive)         score = 2;  // clone fallback
            else                             score = 1;

            if (score > bestScore)
            {
                bestScore = score;
                bestIdx = info.index;
            }
        }

        if (bestIdx != (size_t)-1)
        {
            paths[bestIdx].flags |= DISPLAYCONFIG_PATH_ACTIVE;
            activated.push_back({cible.adapterId, cible.targetId});

            // Enregistrer la source utilisee
            auto& chosenPath = paths[bestIdx];
            usedSources.push_back({chosenPath.sourceInfo.adapterId,
                                    chosenPath.sourceInfo.id});

            Log(L"  -> Activer Path[%zu] (source=%u, score=%d)\n",
                bestIdx, chosenPath.sourceInfo.id, bestScore);
        }
    }

    Log(L"  Cibles activees: %zu\n", activated.size());

    if (activated.empty())
    {
        Log(L"  ERREUR: Aucune cible a activer!\n");
        LogFermer();
        return false;
    }

    // -- Etape 3 : Appliquer la nouvelle topologie --
    bool ok = AppliquerTopologie(paths, modes);

    Log(L"  Resultat: %s\n", ok ? L"OK" : L"ECHEC");
    LogFermer();
    return ok;
}

/* ============================================================================
 * FONCTIONS EXPORTEES
 * ============================================================================ */

GESTIONECRANS_API bool ActiverModeTV()
{
    return BasculerMode(true);
}

GESTIONECRANS_API bool ActiverModeBureau()
{
    return BasculerMode(false);
}

/* ============================================================================
 * DIAGNOSTIC — ObtenirInfoEcrans (affiche dans l'UI C#)
 * ============================================================================ */

static void DiagAppend(wchar_t* buffer, int tailleMax, int* pos,
                       const wchar_t* fmt, ...)
{
    if (*pos >= tailleMax - 2) return;
    wchar_t ligne[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(ligne, 511, _TRUNCATE, fmt, args);
    va_end(args);
    int len = (int)wcslen(ligne);
    if (*pos + len >= tailleMax - 1) return;
    wcscat_s(buffer, tailleMax, ligne);
    *pos += len;
}

GESTIONECRANS_API void ObtenirInfoEcrans(wchar_t* buffer, int tailleMax)
{
    if (!buffer || tailleMax <= 0) return;
    buffer[0] = L'\0';
    int pos = 0;

    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes)
        != ERROR_SUCCESS)
    {
        DiagAppend(buffer, tailleMax, &pos,
            L"ERREUR: GetDisplayConfigBufferSizes\n");
        return;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);

    if (QueryDisplayConfig(QDC_ALL_PATHS,
                           &numPaths, paths.data(),
                           &numModes, modes.data(), nullptr) != ERROR_SUCCESS)
    {
        DiagAppend(buffer, tailleMax, &pos,
            L"ERREUR: QueryDisplayConfig\n");
        return;
    }

    paths.resize(numPaths);
    modes.resize(numModes);

    DiagAppend(buffer, tailleMax, &pos,
        L"=== CCD: %u paths, %u modes ===\n\n",
        numPaths, numModes);

    // Afficher chaque cible UNIQUE (dedupliquee)
    std::vector<TargetId> seen;

    for (size_t i = 0; i < paths.size(); i++)
    {
        auto& path = paths[i];

        if (!path.targetInfo.targetAvailable)
            continue;

        if (TargetDejaVu(seen, path.targetInfo.adapterId,
                         path.targetInfo.id))
            continue;
        seen.push_back({path.targetInfo.adapterId, path.targetInfo.id});

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName;
        if (!ObtenirNomCible(path, targetName))
            continue;

        bool actif = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        Role role = DeterminerRole(
            targetName.monitorFriendlyDeviceName,
            targetName.monitorDevicePath
        );

        const wchar_t* roleStr =
            role == Role::TV     ? L"TV" :
            role == Role::BUREAU ? L"BUREAU" : L"???";

        DiagAppend(buffer, tailleMax, &pos,
            L"[%s] %s | %s\n    %s\n\n",
            actif ? L"ON " : L"OFF",
            targetName.monitorFriendlyDeviceName,
            roleStr,
            targetName.monitorDevicePath
        );
    }

    // -- Ecrire aussi en JSON (diagnostic_ecrans.json) --
    wchar_t jsonPath[MAX_PATH] = {};
    HMODULE hSelf = NULL;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&DiagAppend, &hSelf);
    GetModuleFileNameW(hSelf, jsonPath, MAX_PATH);
    wchar_t* slash = wcsrchr(jsonPath, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(jsonPath, MAX_PATH, L"diagnostic_ecrans.json");

    FILE* f = NULL;
    _wfopen_s(&f, jsonPath, L"w, ccs=UTF-8");
    if (f)
    {
        fwprintf(f, L"{\n  \"ccd_targets\": [\n");

        std::vector<TargetId> jsonSeen;
        bool first = true;

        for (size_t i = 0; i < paths.size(); i++)
        {
            auto& path = paths[i];
            if (!path.targetInfo.targetAvailable) continue;
            if (TargetDejaVu(jsonSeen,
                              path.targetInfo.adapterId,
                              path.targetInfo.id))
                continue;
            jsonSeen.push_back({path.targetInfo.adapterId,
                                 path.targetInfo.id});

            DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
            if (!ObtenirNomCible(path, tn)) continue;

            bool a = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
            Role r = DeterminerRole(
                tn.monitorFriendlyDeviceName,
                tn.monitorDevicePath);

            if (!first) fwprintf(f, L",\n");
            first = false;

            fwprintf(f, L"    {\n");
            fwprintf(f, L"      \"friendly_name\": \"%s\",\n",
                tn.monitorFriendlyDeviceName);
            fwprintf(f, L"      \"device_path\": \"%s\",\n",
                tn.monitorDevicePath);
            fwprintf(f, L"      \"role\": \"%s\",\n",
                r == Role::TV     ? L"TV" :
                r == Role::BUREAU ? L"BUREAU" : L"INCONNU");
            fwprintf(f, L"      \"active\": %s\n",
                a ? L"true" : L"false");
            fwprintf(f, L"    }");
        }

        fwprintf(f, L"\n  ]\n}\n");
        fclose(f);
    }
}

/* ============================================================================
 * POINT D'ENTREE DLL
 * ============================================================================ */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved)
{
    (void)hModule;
    (void)ul_reason_for_call;
    (void)lpReserved;
    return TRUE;
}
