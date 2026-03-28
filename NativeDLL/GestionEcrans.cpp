/*
 * GestionEcrans.cpp
 * -----------------
 * DLL native — moteur CCD agnostique pour gestion multi-écrans.
 *
 * Aucun mot-clé ni rôle hardcodé. Le C# pilote la configuration
 * et transmet les identifiants CCD des cibles à activer.
 *
 * API CCD (Connecting and Configuring Displays) :
 *   1. GetDisplayConfigBufferSizes + QueryDisplayConfig(QDC_ALL_PATHS)
 *      -> lire TOUTE la topologie connue du systeme.
 *   2. DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME)
 *      -> identifier chaque cible (nom EDID, device path).
 *   3. Modifier les flags DISPLAYCONFIG_PATH_ACTIVE sur chaque chemin.
 *   4. SetDisplayConfig(SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
 *      SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE)
 *      -> appliquer la nouvelle topologie d'un seul coup.
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
#include <algorithm>
#include "GestionEcrans.h"

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
    bool   isPrimary;
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
 * LOGIQUE CENTRALE — ActiverEcrans (agnostique)
 * ============================================================================
 *
 * Le C# fournit la liste des cibles a activer (adapterId + targetId).
 * Toutes les autres cibles connues sont desactivees.
 * Les cibles non mentionnees et non connues gardent leur etat.
 *
 * Algorithme en 3 etapes :
 *
 *   1. LIRE la topologie complete (QDC_ALL_PATHS)
 *
 *   2. MODIFIER les flags DISPLAYCONFIG_PATH_ACTIVE :
 *      - Cibles dans la liste -> activer.
 *      - Cibles connues (targetAvailable) non dans la liste -> desactiver.
 *      - Deduplication : un seul chemin actif par cible unique.
 *      - On prefere les chemins deja actifs (modes valides en memoire).
 *      - On assure des sources GPU differentes pour eviter le mode clone.
 *
 *   3. APPLIQUER via SetDisplayConfig avec SDC_ALLOW_CHANGES
 *      -> Windows gere coordonnees, modes et resolutions tout seul.
 */

/* Helper : parser la liste de cibles transmise par le C#.
 * Format : "adLo:adHi:tId:isPrimary\n..."
 */
static std::vector<TargetId> ParserListeCibles(const wchar_t* targetList)
{
    std::vector<TargetId> result;
    if (!targetList || !targetList[0]) return result;

    // Copier pour tokeniser
    wchar_t buf[4096] = {};
    wcsncpy_s(buf, 4096, targetList, 4095);

    wchar_t* ctx = NULL;
    wchar_t* line = wcstok_s(buf, L"\n", &ctx);
    while (line)
    {
        DWORD adLo = 0;
        LONG adHi = 0;
        UINT32 tId = 0;
        int isPrimary = 0;
        if (swscanf_s(line, L"%lu:%ld:%u:%d", &adLo, &adHi, &tId, &isPrimary) == 4)
        {
            TargetId tid = {};
            tid.adapterId.LowPart = adLo;
            tid.adapterId.HighPart = adHi;
            tid.id = tId;
            tid.isPrimary = (isPrimary != 0);
            result.push_back(tid);
        }
        else if (swscanf_s(line, L"%lu:%ld:%u", &adLo, &adHi, &tId) == 3)
        {
            TargetId tid = {};
            tid.adapterId.LowPart = adLo;
            tid.adapterId.HighPart = adHi;
            tid.id = tId;
            tid.isPrimary = false;
            result.push_back(tid);
        }
        line = wcstok_s(NULL, L"\n", &ctx);
    }
    return result;
}

static void LayoutCibles(std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                         std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
                         const std::vector<TargetId>& ciblesVoulues)
{
    if (ciblesVoulues.size() <= 1) return;

    // Trouver le path index correspondants aux ciblesVoulues (qui DOIVENT etre actives)
    std::vector<size_t> pathIdxForCible(ciblesVoulues.size(), (size_t)-1);
    
    for (size_t i = 0; i < ciblesVoulues.size(); i++) {
        for (size_t p = 0; p < paths.size(); p++) {
            if ((paths[p].flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) continue;
            if (LuidEqual(paths[p].targetInfo.adapterId, ciblesVoulues[i].adapterId) &&
                paths[p].targetInfo.id == ciblesVoulues[i].id) {
                pathIdxForCible[i] = p;
                break;
            }
        }
    }

    // Index du primaire
    int primaryIdx = 0;
    for (size_t i = 0; i < ciblesVoulues.size(); i++) {
        if (ciblesVoulues[i].isPrimary) {
            primaryIdx = (int)i; break;
        }
    }

    std::vector<int> posX(ciblesVoulues.size(), 0);
    int curX = 0;

    // A gauche
    for (int i = primaryIdx - 1; i >= 0; i--) {
        size_t pIdx = pathIdxForCible[i];
        if (pIdx == (size_t)-1) continue;
        UINT32 modeIdx = paths[pIdx].sourceInfo.modeInfoIdx;
        if (modeIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || modeIdx >= modes.size()) continue;
        
        curX -= (int)modes[modeIdx].sourceMode.width;
        posX[i] = curX;
    }

    // A droite
    curX = 0;
    for (int i = primaryIdx + 1; i < (int)ciblesVoulues.size(); i++) {
        size_t prevPIdx = pathIdxForCible[i - 1];
        if (prevPIdx != (size_t)-1) {
            UINT32 prevModeIdx = paths[prevPIdx].sourceInfo.modeInfoIdx;
            if (prevModeIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && prevModeIdx < modes.size()) {
                curX += (int)modes[prevModeIdx].sourceMode.width;
            }
        }
        posX[i] = curX;
    }

    // Appliquer positions (uniquement sur X pour l'alignement simple, Y=0)
    for (size_t i = 0; i < ciblesVoulues.size(); i++) {
        size_t pIdx = pathIdxForCible[i];
        if (pIdx == (size_t)-1) continue;
        UINT32 modeIdx = paths[pIdx].sourceInfo.modeInfoIdx;
        if (modeIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || modeIdx >= modes.size()) continue;

        modes[modeIdx].sourceMode.position.x = posX[i];
        modes[modeIdx].sourceMode.position.y = 0;
    }
}

GESTIONECRANS_API bool ActiverEcrans(const wchar_t* targetList)
{
    LogOuvrir();
    Log(L"\n========== ActiverEcrans() ==========\n");

    // -- Parser la liste de cibles voulues --
    std::vector<TargetId> ciblesVoulues = ParserListeCibles(targetList);
    Log(L"  Cibles demandees: %zu\n", ciblesVoulues.size());

    if (ciblesVoulues.empty())
    {
        Log(L"  ERREUR: Liste de cibles vide!\n");
        LogFermer();
        return false;
    }

    // -- Etape 1 : Lire la topologie complete --
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;

    if (!LireTopologie(paths, modes))
    {
        LogFermer();
        return false;
    }

    // -- Etape 2 : Inventorier chaque chemin disponible --
    struct PathInfo
    {
        size_t  index;
        bool    wasActive;
        LUID    adapterId;
        UINT32  targetId;
        LUID    sourceAdapterId;
        UINT32  sourceId;
        wchar_t friendlyName[64];
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

        bool wasActive = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;

        Log(L"  Path[%zu]: \"%s\" | %s\n",
            i,
            targetName.monitorFriendlyDeviceName,
            wasActive ? L"ACTIF" : L"inactif"
        );

        PathInfo pi = {};
        pi.index = i;
        pi.wasActive = wasActive;
        pi.adapterId = path.targetInfo.adapterId;
        pi.targetId = path.targetInfo.id;
        pi.sourceAdapterId = path.sourceInfo.adapterId;
        pi.sourceId = path.sourceInfo.id;
        wcsncpy_s(pi.friendlyName, 64, targetName.monitorFriendlyDeviceName, 63);
        infos.push_back(pi);
    }

    // -- Etape 2a : Desactiver tous les chemins vers des cibles connues --
    for (auto& info : infos)
        paths[info.index].flags &= ~DISPLAYCONFIG_PATH_ACTIVE;

    // -- Etape 2b : Activer les chemins vers les cibles demandees --
    //
    // EXTEND (pas clone) :
    //   Deux chemins CCD partageant la MEME source GPU = clone.
    //   Deux chemins CCD avec des sources DIFFERENTES = extend.
    //   On s'assure que chaque cible utilise une source GPU unique.

    std::vector<TargetId> activated;
    std::vector<TargetId> usedSources;

    auto sourceDejaUtilisee = [&](LUID sAdapterId, UINT32 sId) -> bool {
        for (const auto& s : usedSources)
            if (LuidEqual(s.adapterId, sAdapterId) && s.id == sId)
                return true;
        return false;
    };

    bool requiresPass2 = false;

    // Pour chaque cible voulue (dans l'ordre fourni par le C#),
    // trouver le meilleur chemin CCD.
    // Priorite : (1) deja actif + source libre, (2) inactif + source libre,
    //            (3) deja actif (meme si source prise), (4) inactif
    for (auto& cible : ciblesVoulues)
    {
        if (TargetDejaVu(activated, cible.adapterId, cible.id))
            continue;

        size_t bestIdx = (size_t)-1;
        int    bestScore = -1;
        bool   wasActive = false;

        for (auto& info : infos)
        {
            if (!LuidEqual(info.adapterId, cible.adapterId) ||
                info.targetId != cible.id)
                continue;

            bool srcLibre = !sourceDejaUtilisee(info.sourceAdapterId, info.sourceId);
            int score = 0;
            if (info.wasActive && srcLibre)       score = 4;  // ideal
            else if (!info.wasActive && srcLibre)  score = 3;
            else if (info.wasActive)               score = 2;  // clone fallback
            else                                   score = 1;

            if (score > bestScore)
            {
                bestScore = score;
                bestIdx = info.index;
                wasActive = info.wasActive;
            }
        }

        if (bestIdx != (size_t)-1)
        {
            paths[bestIdx].flags |= DISPLAYCONFIG_PATH_ACTIVE;
            activated.push_back(cible);

            auto& chosenPath = paths[bestIdx];
            usedSources.push_back({chosenPath.sourceInfo.adapterId,
                                    chosenPath.sourceInfo.id, false});

            if (!wasActive) requiresPass2 = true;

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
    bool ok = true;
    if (requiresPass2) {
        // Pass 1: Reveiller les ecrans 
        ok = AppliquerTopologie(paths, modes);
        if (ok && ciblesVoulues.size() > 1) {
            // Pass 2: Relire la topologie (maintenant active, modes valides) et forcer le layout
            if (LireTopologie(paths, modes)) {
                LayoutCibles(paths, modes, ciblesVoulues);
                ok = AppliquerTopologie(paths, modes);
            }
        }
    } else {
        // Tous les ecrans sont deja actifs ! Layout direct sans clignotement inutile
        if (ciblesVoulues.size() > 1) {
            LayoutCibles(paths, modes, ciblesVoulues);
        }
        ok = AppliquerTopologie(paths, modes);
    }

    Log(L"  Resultat: %s\n", ok ? L"OK" : L"ECHEC");
    LogFermer();
    return ok;
}

/* ============================================================================
 * FONCTIONS EXPORTEES — EnumererEcrans
 * ============================================================================
 *
 * Retourne TOUS les ecrans CCD disponibles (dedupliques par cible unique).
 * Format par ligne (separees par \n) :
 *   targetId|adapterId_lo|adapterId_hi|friendlyName|devicePath|active
 */

GESTIONECRANS_API void EnumererEcrans(wchar_t* buffer, int tailleMax)
{
    if (!buffer || tailleMax <= 0) return;
    buffer[0] = L'\0';
    int pos = 0;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!LireTopologie(paths, modes)) return;

    std::vector<TargetId> seen;

    for (size_t i = 0; i < paths.size(); i++)
    {
        auto& path = paths[i];
        if (!path.targetInfo.targetAvailable) continue;
        if (TargetDejaVu(seen, path.targetInfo.adapterId, path.targetInfo.id))
            continue;
        seen.push_back({path.targetInfo.adapterId, path.targetInfo.id});

        DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
        if (!ObtenirNomCible(path, tn)) continue;

        bool actif = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;

        // Format : targetId|adapterId_lo|adapterId_hi|friendlyName|devicePath|active
        wchar_t ligne[1024];
        _snwprintf_s(ligne, 1023, _TRUNCATE,
            L"%u|%lu|%ld|%s|%s|%d\n",
            path.targetInfo.id,
            path.targetInfo.adapterId.LowPart,
            (long)path.targetInfo.adapterId.HighPart,
            tn.monitorFriendlyDeviceName,
            tn.monitorDevicePath,
            actif ? 1 : 0
        );

        int len = (int)wcslen(ligne);
        if (pos + len >= tailleMax - 1) break;
        wcscat_s(buffer, tailleMax, ligne);
        pos += len;
    }
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

        DiagAppend(buffer, tailleMax, &pos,
            L"[%s] %s\n    id=%u  adapter=%lu:%ld\n    %s\n\n",
            actif ? L"ON " : L"OFF",
            targetName.monitorFriendlyDeviceName,
            path.targetInfo.id,
            path.targetInfo.adapterId.LowPart,
            (long)path.targetInfo.adapterId.HighPart,
            targetName.monitorDevicePath
        );
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
