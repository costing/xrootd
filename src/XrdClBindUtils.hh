//------------------------------------------------------------------------------
// Copyright (c) 2012-2013 by European Organization for Nuclear Research (CERN)
// Author: Justin Salmon <jsalmon@cern.ch>
//------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------

#ifndef XRDCLBINDUTILS_HH_
#define XRDCLBINDUTILS_HH_

#include <Python.h>

#include "XrdCl/XrdClXRootDResponses.hh"

#include "ClientType.hh"
#include "URLType.hh"
#include "StatInfoType.hh"
#include "HostInfoType.hh"

namespace XrdClBind
{
  //----------------------------------------------------------------------------
  //! Convert a C++ type to its corresponding Python binding type. We cast
  //! the object to a void * before packing it into a PyCObject.
  //!
  //! Note: The PyCObject API is deprecated as of Python 2.7
  //----------------------------------------------------------------------------
  template<class Type>
  static PyObject* ConvertType( Type *type, PyTypeObject *bindType )
  {
    PyObject *args = Py_BuildValue( "(O)",
                     PyCObject_FromVoidPtr( (void *) type, NULL) );
    if ( !args )
      return NULL;

    bindType->tp_new = PyType_GenericNew;
    if ( PyType_Ready( bindType ) < 0 ) {
      return NULL;
    }

    PyObject *bind = PyObject_CallObject( (PyObject *) bindType, args );
    Py_DECREF( args );
    if ( !bind )
      return NULL;

    return bind;
  }

  //----------------------------------------------------------------------------
  //! Convert an XRootDStatus object to a Python dictionary
  //----------------------------------------------------------------------------
  static PyObject* XRootDStatusDict( XrdCl::XRootDStatus *status )
  {
    PyObject *dict = Py_BuildValue( "{sHsHsIsssisOsOsO}",
        "status",    status->status,
        "code",      status->code,
        "errNo",     status->errNo,
        "message",   status->ToStr().c_str(),
        "shellCode", status->GetShellCode(),
        "isError",   PyBool_FromLong( status->IsError() ),
        "isFatal",   PyBool_FromLong( status->IsFatal() ),
        "isOK",      PyBool_FromLong( status->IsOK() ) );

    return (dict == NULL || PyErr_Occurred()) ? NULL : dict;
  }

  //----------------------------------------------------------------------------
  //! Check that the given callback is actually callable.
  //----------------------------------------------------------------------------
  static bool IsCallable( PyObject *callable )
  {
    if ( !PyCallable_Check( callable ) ) {
      PyErr_SetString( PyExc_TypeError, "parameter must be callable" );
      return NULL;
    }
    // We need to keep this callback
    Py_INCREF (callable);
    return true;
  }

  //----------------------------------------------------------------------------
  //! Initialize the Python types for the extension.
  //----------------------------------------------------------------------------
  static int InitTypes()
  {
    //ClientType.tp_new   = PyType_GenericNew;
    URLType.tp_new      = PyType_GenericNew;
    StatInfoType.tp_new = PyType_GenericNew;
    HostInfoType.tp_new = PyType_GenericNew;

    //if ( PyType_Ready( &ClientType ) < 0 )   return -1;
    if ( PyType_Ready( &URLType ) < 0 )      return -1;
    if ( PyType_Ready( &StatInfoType ) < 0 ) return -1;
    if ( PyType_Ready( &HostInfoType ) < 0 ) return -1;

    //Py_INCREF( &ClientType );
    Py_INCREF( &URLType );
    Py_INCREF( &StatInfoType );
    Py_INCREF( &HostInfoType );

    return 0;
  }
}

#endif /* XRDCLBINDUTILS_HH_ */
