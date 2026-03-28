#pragma once

/*
 * GestionEcrans.h
 * ---------------
 * Header pour la DLL native de gestion multi-écrans.
 *
 * API MODERNE CCD (Connecting and Configuring Displays) :
 *   Utilise QueryDisplayConfig / SetDisplayConfig au lieu de l'ancienne
 *   API ChangeDisplaySettingsExW. Élimine les conflits de coordonnées
 *   et les problèmes de topologie.
 *
 * Cible : Windows 10 et supérieur.
 *
 * Utilisation C# :
 *   [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl)]
 *   static extern bool ActiverModeTV();
 *
 *   [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl)]
 *   static extern bool ActiverModeBureau();
 *
 *   [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl)]
 *   static extern void ObtenirInfoEcrans(
 *       [MarshalAs(UnmanagedType.LPWStr)] StringBuilder buffer, int tailleMax);
 */

#ifdef GESTIONECRANS_EXPORTS
    #define GESTIONECRANS_API extern "C" __declspec(dllexport)
#else
    #define GESTIONECRANS_API extern "C" __declspec(dllimport)
#endif

/*
 * ActiverModeTV
 * -------------
 * Lit la topologie CCD, active le chemin vers la TV (HEC/HISENSE),
 * désactive les chemins vers les LG (GSM). Applique via SetDisplayConfig.
 */
GESTIONECRANS_API bool ActiverModeTV();

/*
 * ActiverModeBureau
 * -----------------
 * Lit la topologie CCD, active les chemins vers les LG (GSM),
 * désactive le chemin vers la TV (HEC/HISENSE). Applique via SetDisplayConfig.
 */
GESTIONECRANS_API bool ActiverModeBureau();

/*
 * ObtenirInfoEcrans
 * -----------------
 * Diagnostic CCD : remplit un buffer avec la liste de tous les chemins
 * d'affichage, leur nom cible, état ACTIVE, et rôle assigné.
 */
GESTIONECRANS_API void ObtenirInfoEcrans(wchar_t* buffer, int tailleMax);

/*
 * ObtenirEcransBureau
 * -------------------
 * Retourne les noms conviviaux des écrans BUREAU détectés, séparés par '|'.
 * Ex: "23EA53|LG FULL HD"
 */
GESTIONECRANS_API void ObtenirEcransBureau(wchar_t* buffer, int tailleMax);

/*
 * ObtenirLayoutConfig
 * -------------------
 * Retourne la config layout actuelle : "nom1|nom2|...\nprimary_index"
 * Chaîne vide si pas de layout.json.
 */
GESTIONECRANS_API void ObtenirLayoutConfig(wchar_t* buffer, int tailleMax);

/*
 * DefinirOrdreBureau
 * ------------------
 * Sauvegarde l'ordre des écrans bureau dans layout.json.
 * Format attendu : "nom1|nom2|...\nprimary_index"
 */
GESTIONECRANS_API void DefinirOrdreBureau(const wchar_t* config);
