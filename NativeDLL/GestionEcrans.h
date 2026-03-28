#pragma once

/*
 * GestionEcrans.h
 * ---------------
 * Header pour la DLL native de gestion multi-écrans.
 *
 * MOTEUR CCD AGNOSTIQUE (Connecting and Configuring Displays) :
 *   Utilise QueryDisplayConfig / SetDisplayConfig.
 *   Aucun mot-clé ni rôle hardcodé : le C# pilote la configuration.
 *
 * Cible : Windows 10 et supérieur.
 *
 * Utilisation C# :
 *   [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
 *   static extern void EnumererEcrans(StringBuilder buffer, int tailleMax);
 *
 *   [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
 *   static extern bool ActiverEcrans(string targetList);
 *
 *   [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
 *   static extern void ObtenirInfoEcrans(StringBuilder buffer, int tailleMax);
 */

#ifdef GESTIONECRANS_EXPORTS
    #define GESTIONECRANS_API extern "C" __declspec(dllexport)
#else
    #define GESTIONECRANS_API extern "C" __declspec(dllimport)
#endif

/*
 * EnumererEcrans
 * --------------
 * Retourne TOUS les écrans CCD disponibles (dédupliqués par cible unique).
 * Format par ligne (séparées par \n) :
 *   targetId|adapterId_lo|adapterId_hi|friendlyName|devicePath|active
 * Où active = "1" ou "0".
 */
GESTIONECRANS_API void EnumererEcrans(wchar_t* buffer, int tailleMax);

/*
 * ActiverEcrans
 * -------------
 * Reçoit la liste des cibles à activer, séparées par \n.
 * Format par entrée : "adapterId_lo:adapterId_hi:targetId"
 * Désactive tous les chemins vers des cibles connues qui ne sont pas
 * dans la liste. Les cibles inconnues (non listées, ex: VR) gardent
 * leur état. Applique via SetDisplayConfig.
 */
GESTIONECRANS_API bool ActiverEcrans(const wchar_t* targetList);

/*
 * ObtenirInfoEcrans
 * -----------------
 * Diagnostic CCD : remplit un buffer avec la liste de tous les chemins
 * d'affichage, leur nom cible et état ACTIVE (sans rôle — agnostique).
 */
GESTIONECRANS_API void ObtenirInfoEcrans(wchar_t* buffer, int tailleMax);
